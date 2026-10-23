/*
 * Smart Plant Irrigation System - SOIL-ONLY version (no WiFi, no weather API)
 * NSS Project, IIT Tirupati
 *
 * Rule: water when soil moisture < START threshold, stop at >= STOP threshold.
 * Also stops on safety timeout, daily cap, or sensor fault.
 *
 * Hardware:
 *   2x capacitive soil moisture sensor V2.0 -> GPIO 32, 33 (ADC1)
 *     (GPIO 34, 35 are reserved for spare / later sensors, see SOIL_PINS)
 *   1-ch relay module (low-level trigger)   -> GPIO 26
 *   Solenoid valve 12 V via relay, with 1N4007 flyback diode across the valve coil
 *
 * Libraries: none needed beyond the ESP32 board package.
 *
 * Serial monitor (115200 baud) commands:
 *   d = save current readings as DRY calibration (sensors in open air)
 *   w = save current readings as WET calibration (sensors in water)
 *   p = print calibration values
 *   r = reset calibration to defaults
 *   o = manual valve open test (runs 5 s, only when idle)
 */

#include <Preferences.h>
#include <esp_task_wdt.h>

// ============================== CONFIG ======================================

// --- Pins ---
// Active soil probes (ADC1 channels). To add spares later: {32, 33, 34, 35}
const uint8_t SOIL_PINS[]  = {32, 33};
const uint8_t NUM_SENSORS  = sizeof(SOIL_PINS) / sizeof(SOIL_PINS[0]);
const uint8_t RELAY_PIN    = 26;
const bool    RELAY_ACTIVE_LOW = true;           // low-level trigger relay

// --- Irrigation thresholds (from project report) ---
const float START_MOISTURE_PCT = 48.5;   // start watering below this
const float STOP_MOISTURE_PCT  = 80.0;   // stop watering at/above this

// --- Safety limits ---
const unsigned long MAX_RUN_MS   = 15UL * 60UL * 1000UL;   // 15 min per cycle
const unsigned long MIN_OFF_MS   = 10UL * 60UL * 1000UL;   // rest between cycles
const unsigned long DAILY_MAX_MS = 60UL * 60UL * 1000UL;   // 60 min per 24 h

// --- Timing ---
// Soil is checked once per hour when idle (less heat, less wear). While the valve is
// open it is checked every few seconds so watering still stops at the upper threshold.
const unsigned long IDLE_CHECK_INTERVAL_MS     = 60UL * 60UL * 1000UL;  // 1 hour
const unsigned long WATERING_CHECK_INTERVAL_MS = 5000;                  // 5 s while watering
const unsigned long LOG_INTERVAL_MS            = 10000;
const uint32_t      CPU_FREQ_MHZ = 80;   // lower clock = less heat (no WiFi, could go to 40)
const uint32_t      WDT_TIMEOUT_S       = 30;

// --- Sensor sanity / default calibration ---
const int ADC_MIN_VALID = 200;     // below this -> probe unplugged / shorted
const int ADC_MAX_VALID = 3900;    // above this -> probe unplugged / floating high
const int DEFAULT_DRY   = 3200;    // raw reading in air   (CALIBRATE YOURS!)
const int DEFAULT_WET   = 1400;    // raw reading in water (CALIBRATE YOURS!)

// ============================== STATE =======================================

enum State { IDLE, WATERING };

Preferences prefs;

int dryRaw[NUM_SENSORS];
int wetRaw[NUM_SENSORS];

State state = IDLE;
unsigned long valveOpenedAt = 0;
unsigned long lastStopAt    = 0;
bool          everStopped   = false;

unsigned long dailyWateredMs = 0;
unsigned long dayStartAt     = 0;

float   moisturePct = NAN;
uint8_t validProbes = 0;

unsigned long lastControlAt = 0;
bool          firstCheck    = true;   // run one soil check right at boot
unsigned long lastLogAt     = 0;

String lastReason = "boot";

// ============================== RELAY / VALVE ===============================

inline void relayWrite(bool on) {
  digitalWrite(RELAY_PIN, (on ^ RELAY_ACTIVE_LOW) ? HIGH : LOW);
}

void openValve(const char* why) {
  if (state == WATERING) return;
  relayWrite(true);
  state = WATERING;
  valveOpenedAt = millis();
  lastReason = why;
  Serial.printf("[VALVE] OPEN  - %s\n", why);
}

void closeValve(const char* why) {
  if (state == IDLE) {
    relayWrite(false);               // make sure it's really off
    return;
  }
  relayWrite(false);
  dailyWateredMs += millis() - valveOpenedAt;
  state = IDLE;
  lastStopAt = millis();
  everStopped = true;
  lastReason = why;
  Serial.printf("[VALVE] CLOSE - %s\n", why);
}

// ============================== CALIBRATION =================================

void loadCalibration() {
  prefs.begin("irrig", true);
  for (uint8_t i = 0; i < NUM_SENSORS; i++) {
    char kd[8], kw[8];
    snprintf(kd, sizeof(kd), "dry%u", i);
    snprintf(kw, sizeof(kw), "wet%u", i);
    dryRaw[i] = prefs.getInt(kd, DEFAULT_DRY);
    wetRaw[i] = prefs.getInt(kw, DEFAULT_WET);
  }
  prefs.end();
}

void saveCalibration() {
  prefs.begin("irrig", false);
  for (uint8_t i = 0; i < NUM_SENSORS; i++) {
    char kd[8], kw[8];
    snprintf(kd, sizeof(kd), "dry%u", i);
    snprintf(kw, sizeof(kw), "wet%u", i);
    prefs.putInt(kd, dryRaw[i]);
    prefs.putInt(kw, wetRaw[i]);
  }
  prefs.end();
}

void printCalibration() {
  Serial.println("[CAL] sensor  pin  dry  wet");
  for (uint8_t i = 0; i < NUM_SENSORS; i++)
    Serial.printf("[CAL] %u  GPIO%u  %d  %d\n", i, SOIL_PINS[i], dryRaw[i], wetRaw[i]);
}

// ============================== SENSORS =====================================

int readRawAveraged(uint8_t pin) {
  long sum = 0;
  const int N = 16;
  for (int i = 0; i < N; i++) {
    sum += analogRead(pin);
    delayMicroseconds(200);
  }
  return (int)(sum / N);
}

// Reads all probes, averages the valid ones into moisturePct.
void readSoil() {
  float total = 0;
  validProbes = 0;
  for (uint8_t i = 0; i < NUM_SENSORS; i++) {
    int raw = readRawAveraged(SOIL_PINS[i]);
    if (raw < ADC_MIN_VALID || raw > ADC_MAX_VALID) continue;   // faulty probe
    if (dryRaw[i] == wetRaw[i]) continue;                       // bad calibration
    float pct = (float)(dryRaw[i] - raw) * 100.0f / (float)(dryRaw[i] - wetRaw[i]);
    pct = constrain(pct, 0.0f, 100.0f);
    total += pct;
    validProbes++;
  }
  moisturePct = validProbes ? total / validProbes : NAN;
}

// Records current raw readings as dry or wet calibration (serial commands d / w).
void calibrateNow(bool dry) {
  for (uint8_t i = 0; i < NUM_SENSORS; i++) {
    int raw = readRawAveraged(SOIL_PINS[i]);
    if (dry) dryRaw[i] = raw; else wetRaw[i] = raw;
  }
  saveCalibration();
  Serial.printf("[CAL] saved %s values\n", dry ? "DRY" : "WET");
  printCalibration();
}

// ============================== CONTROL LOGIC ===============================

void runControl() {
  unsigned long now = millis();

  // Reset the daily watering budget every 24 h of uptime.
  if (now - dayStartAt >= 24UL * 60UL * 60UL * 1000UL) {
    dayStartAt = now;
    dailyWateredMs = 0;
  }

  readSoil();

  bool soilOk = validProbes > 0 && !isnan(moisturePct);
  unsigned long runningMs = (state == WATERING) ? (now - valveOpenedAt) : 0;
  bool dailyCapHit = (dailyWateredMs + runningMs) >= DAILY_MAX_MS;

  if (state == WATERING) {
    if (!soilOk)                               closeValve("STOP: soil sensor fault");
    else if (moisturePct >= STOP_MOISTURE_PCT) closeValve("STOP: upper moisture reached");
    else if (runningMs >= MAX_RUN_MS)          closeValve("STOP: safety timeout");
    else if (dailyCapHit)                      closeValve("STOP: daily limit reached");
  } else {
    if (!soilOk) {
      lastReason = "IDLE: soil sensor fault";
    } else if (moisturePct < START_MOISTURE_PCT) {
      bool restedEnough = !everStopped || (now - lastStopAt >= MIN_OFF_MS);
      if (dailyCapHit)         lastReason = "IDLE: daily limit reached";
      else if (!restedEnough)  lastReason = "IDLE: waiting min off-time";
      else                     openValve("START: soil dry");
    } else {
      lastReason = "IDLE: soil moisture OK";
    }
  }
}

// ============================== LOGGING =====================================

void logStatus() {
  Serial.printf("[LOG] t=%lus soil=%.1f%% (%u/%u probes) valve=%s daily=%lus | %s\n",
                millis() / 1000, moisturePct, validProbes, NUM_SENSORS,
                state == WATERING ? "OPEN" : "closed",
                (dailyWateredMs + (state == WATERING ? millis() - valveOpenedAt : 0)) / 1000,
                lastReason.c_str());
}

// ============================== SERIAL COMMANDS =============================

void handleSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    switch (c) {
      case 'd': calibrateNow(true);  break;
      case 'w': calibrateNow(false); break;
      case 'p': printCalibration();  break;
      case 'r':
        for (uint8_t i = 0; i < NUM_SENSORS; i++) { dryRaw[i] = DEFAULT_DRY; wetRaw[i] = DEFAULT_WET; }
        saveCalibration();
        Serial.println("[CAL] reset to defaults");
        break;
      case 'o':
        if (state == IDLE) {
          Serial.println("[TEST] valve open for 5 s");
          openValve("manual test");
          unsigned long t0 = millis();
          while (millis() - t0 < 5000) { delay(100); esp_task_wdt_reset(); }
          closeValve("manual test done");
        }
        break;
      default: break;
    }
  }
}

// ============================== SETUP / LOOP ================================

void setupWatchdog() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  esp_task_wdt_config_t cfg = {};
  cfg.timeout_ms = WDT_TIMEOUT_S * 1000;
  cfg.idle_core_mask = 0;
  cfg.trigger_panic = true;
  esp_task_wdt_reconfigure(&cfg);
#else
  esp_task_wdt_init(WDT_TIMEOUT_S, true);
#endif
  esp_task_wdt_add(NULL);
}

void setup() {
  // Valve must be OFF before anything else happens.
  digitalWrite(RELAY_PIN, RELAY_ACTIVE_LOW ? HIGH : LOW);
  pinMode(RELAY_PIN, OUTPUT);
  relayWrite(false);

  setCpuFrequencyMhz(CPU_FREQ_MHZ);   // lower clock = less heat
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== Smart Plant Irrigation System (soil-only) ===");

  analogReadResolution(12);
  for (uint8_t i = 0; i < NUM_SENSORS; i++) {
    pinMode(SOIL_PINS[i], INPUT);
    analogSetPinAttenuation(SOIL_PINS[i], ADC_11db);   // full 0-3.3 V range
  }

  loadCalibration();
  printCalibration();

  setupWatchdog();

  dayStartAt = millis();
  Serial.println("Commands: d=dry cal, w=wet cal, p=print cal, r=reset cal, o=valve test");
}

void loop() {
  esp_task_wdt_reset();
  unsigned long now = millis();

  handleSerial();

  // Soil check: hourly when idle, every few seconds while watering.
  unsigned long checkInterval = (state == WATERING) ? WATERING_CHECK_INTERVAL_MS
                                                    : IDLE_CHECK_INTERVAL_MS;
  if (firstCheck || now - lastControlAt >= checkInterval) {
    firstCheck = false;
    lastControlAt = now;
    State before = state;
    runControl();
    if (now - lastLogAt >= LOG_INTERVAL_MS || state != before) {
      lastLogAt = now;
      logStatus();
    }
  }

  delay(10);
}
