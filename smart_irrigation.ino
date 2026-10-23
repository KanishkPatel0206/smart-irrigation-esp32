/*
 * Smart Plant Irrigation System - ESP32 firmware
 * NSS Project, IIT Tirupati
 *
 * Rule: irrigate when soil moisture < START threshold AND rain probability < 40 %.
 * Stop when moisture >= STOP threshold, rain probability >= 40 %, safety timeout
 * hits, a daily cap is reached, or a sensor / weather fault is detected.
 *
 * Hardware:
 *   2x capacitive soil moisture sensor V2.0 -> GPIO 32, 33 (ADC1 only!)
 *     (GPIO 34, 35 are reserved for backup / later sensors, see SOIL_PINS)
 *   DHT22                                    -> GPIO 4
 *   1-ch relay module (low-level trigger)    -> GPIO 26
 *   Solenoid valve 12 V via relay, with 1N4007 flyback diode across the valve coil
 *
 * Libraries (Library Manager):
 *   - "DHT sensor library" by Adafruit (+ "Adafruit Unified Sensor")
 *   - "ArduinoJson" by Benoit Blanchon, version 7.x
 *
 * Serial monitor (115200 baud) commands:
 *   d = save current readings as DRY calibration (sensors in open air)
 *   w = save current readings as WET calibration (sensors in water)
 *   p = print calibration values
 *   r = reset calibration to defaults
 *   o = manual valve open test (runs 5 s, only when idle)
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <DHT.h>
#include <Preferences.h>
#include <esp_task_wdt.h>

// ============================== CONFIG ======================================

// --- WiFi / weather API ---
const char* WIFI_SSID     = "YOUR_WIFI_NAME";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
const char* OWM_API_KEY   = "YOUR_OPENWEATHERMAP_KEY";
const char* LATITUDE      = "13.5600";   // set to your plot's location
const char* LONGITUDE     = "79.5300";
const int   FORECAST_ENTRIES = 2;        // 3-hour steps -> 2 = next ~6 hours

// --- Pins ---
// Active soil probes (ADC1 channels, work with WiFi).
// To add the spare probes later, just extend the list: {32, 33, 34, 35}
const uint8_t SOIL_PINS[]  = {32, 33};
const uint8_t NUM_SENSORS  = sizeof(SOIL_PINS) / sizeof(SOIL_PINS[0]);
const uint8_t DHT_PIN      = 4;
const uint8_t RELAY_PIN    = 26;
const bool    RELAY_ACTIVE_LOW = true;           // low-level trigger relay

// --- Irrigation thresholds (from project report) ---
const float START_MOISTURE_PCT = 48.5;   // start watering below this
const float STOP_MOISTURE_PCT  = 80.0;   // stop watering at/above this
const float RAIN_LIMIT_PCT     = 40.0;   // no watering if rain probability >= this

// --- Safety limits ---
const unsigned long MAX_RUN_MS        = 15UL * 60UL * 1000UL;      // 15 min per cycle
const unsigned long MIN_OFF_MS        = 10UL * 60UL * 1000UL;      // rest between cycles
const unsigned long DAILY_MAX_MS      = 60UL * 60UL * 1000UL;      // 60 min per 24 h
const unsigned long WEATHER_STALE_MS  = 2UL * 60UL * 60UL * 1000UL; // forecast max age
const bool ALLOW_WATERING_WITHOUT_WEATHER = false;  // false = fail safe (valve stays shut)

// --- Timing ---
const unsigned long CONTROL_INTERVAL_MS = 2000;
const unsigned long LOG_INTERVAL_MS     = 10000;
const unsigned long WEATHER_INTERVAL_MS = 15UL * 60UL * 1000UL;    // 15 min
const unsigned long WIFI_RETRY_MS       = 30000;
const uint32_t      WDT_TIMEOUT_S       = 30;

// --- Sensor sanity / default calibration ---
const int ADC_MIN_VALID = 200;     // below this -> probe unplugged / shorted
const int ADC_MAX_VALID = 3900;    // above this -> probe unplugged / floating high
const int DEFAULT_DRY   = 3200;    // raw reading in air   (CALIBRATE YOURS!)
const int DEFAULT_WET   = 1400;    // raw reading in water (CALIBRATE YOURS!)

// ============================== STATE =======================================

enum State { IDLE, WATERING };

DHT dht(DHT_PIN, DHT22);
Preferences prefs;

int dryRaw[NUM_SENSORS];
int wetRaw[NUM_SENSORS];

State state = IDLE;
unsigned long valveOpenedAt = 0;
unsigned long lastStopAt    = 0;
bool          everStopped   = false;

unsigned long dailyWateredMs = 0;
unsigned long dayStartAt     = 0;

float moisturePct   = NAN;
uint8_t validProbes = 0;
float airTempC      = NAN;
float airHumidity   = NAN;

float rainPct = 0;
bool  weatherEverOk = false;
unsigned long lastWeatherOkAt = 0;
unsigned long lastWeatherTry  = 0;

unsigned long lastControlAt = 0;
unsigned long lastLogAt     = 0;
unsigned long lastWifiTry   = 0;

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
  Serial.println("[CAL] sensor  dry  wet");
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

void readDht() {
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  if (!isnan(t)) airTempC = t;
  if (!isnan(h)) airHumidity = h;
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

// ============================== WEATHER =====================================

// Returns true and sets popPercent (max rain probability over next entries).
bool fetchRainProbability(float &popPercent) {
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClientSecure client;
  client.setInsecure();   // skips certificate check; pin a root CA for production
  HTTPClient http;

  String url = String("https://api.openweathermap.org/data/2.5/forecast?lat=") + LATITUDE +
               "&lon=" + LONGITUDE + "&cnt=" + FORECAST_ENTRIES +
               "&units=metric&appid=" + OWM_API_KEY;

  if (!http.begin(client, url)) return false;
  http.setTimeout(8000);

  int code = http.GET();
  if (code != 200) {
    Serial.printf("[WEATHER] HTTP error %d\n", code);
    http.end();
    return false;
  }

  JsonDocument filter;
  filter["list"][0]["pop"] = true;

  JsonDocument doc;
  DeserializationError err =
      deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();

  if (err) {
    Serial.printf("[WEATHER] JSON error: %s\n", err.c_str());
    return false;
  }

  JsonArray list = doc["list"].as<JsonArray>();
  if (list.isNull() || list.size() == 0) return false;

  float maxPop = 0;
  for (JsonObject item : list) {
    float p = item["pop"] | 0.0f;       // 0.0 - 1.0
    if (p > maxPop) maxPop = p;
  }
  popPercent = maxPop * 100.0f;
  return true;
}

void updateWeather() {
  lastWeatherTry = millis();
  float pop;
  if (fetchRainProbability(pop)) {
    rainPct = pop;
    weatherEverOk = true;
    lastWeatherOkAt = millis();
    Serial.printf("[WEATHER] rain probability = %.0f %%\n", rainPct);
  } else {
    Serial.println("[WEATHER] fetch failed, keeping last value");
  }
}

bool weatherFresh() {
  return weatherEverOk && (millis() - lastWeatherOkAt < WEATHER_STALE_MS);
}

// ============================== WIFI ========================================

void connectWifi(bool blocking) {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  if (!blocking) return;
  Serial.print("[WIFI] connecting");
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) {
    delay(500);
    Serial.print(".");
    esp_task_wdt_reset();
  }
  if (WiFi.status() == WL_CONNECTED)
    Serial.printf("\n[WIFI] connected, IP %s\n", WiFi.localIP().toString().c_str());
  else
    Serial.println("\n[WIFI] not connected (will retry)");
}

void maintainWifi() {
  if (WiFi.status() == WL_CONNECTED) return;
  if (millis() - lastWifiTry < WIFI_RETRY_MS) return;
  lastWifiTry = millis();
  Serial.println("[WIFI] reconnecting...");
  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
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
  bool wxOk   = weatherFresh();
  // When the forecast is unavailable, either assume "no rain" or fail safe.
  float effectiveRain = wxOk ? rainPct : (ALLOW_WATERING_WITHOUT_WEATHER ? 0.0f : 100.0f);

  unsigned long runningMs = (state == WATERING) ? (now - valveOpenedAt) : 0;
  bool dailyCapHit = (dailyWateredMs + runningMs) >= DAILY_MAX_MS;

  if (state == WATERING) {
    if (!soilOk)                               closeValve("STOP: soil sensor fault");
    else if (moisturePct >= STOP_MOISTURE_PCT) closeValve("STOP: upper moisture reached");
    else if (effectiveRain >= RAIN_LIMIT_PCT)  closeValve(wxOk ? "STOP: rain expected"
                                                               : "STOP: weather data stale");
    else if (runningMs >= MAX_RUN_MS)          closeValve("STOP: safety timeout");
    else if (dailyCapHit)                      closeValve("STOP: daily limit reached");
  } else {
    if (!soilOk) {
      lastReason = "IDLE: soil sensor fault";
    } else if (moisturePct < START_MOISTURE_PCT) {
      bool restedEnough = !everStopped || (now - lastStopAt >= MIN_OFF_MS);
      if (effectiveRain >= RAIN_LIMIT_PCT)
        lastReason = wxOk ? "IDLE: dry but rain expected" : "IDLE: dry but no fresh forecast";
      else if (dailyCapHit)    lastReason = "IDLE: daily limit reached";
      else if (!restedEnough)  lastReason = "IDLE: waiting min off-time";
      else                     openValve("START: soil dry, low rain chance");
    } else {
      lastReason = "IDLE: soil moisture OK";
    }
  }
}

// ============================== LOGGING =====================================

void logStatus() {
  readDht();
  Serial.printf("[LOG] t=%lus soil=%.1f%% (%u/%u probes) air=%.1fC hum=%.0f%% rain=%.0f%%%s "
                "valve=%s daily=%lus | %s\n",
                millis() / 1000, moisturePct, validProbes, NUM_SENSORS,
                airTempC, airHumidity, rainPct, weatherFresh() ? "" : "(stale)",
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

  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== Smart Plant Irrigation System ===");

  analogReadResolution(12);
  for (uint8_t i = 0; i < NUM_SENSORS; i++) {
    pinMode(SOIL_PINS[i], INPUT);
    analogSetPinAttenuation(SOIL_PINS[i], ADC_11db);   // full 0-3.3 V range
  }
  dht.begin();

  loadCalibration();
  printCalibration();

  setupWatchdog();
  connectWifi(true);
  updateWeather();

  dayStartAt = millis();
  Serial.println("Commands: d=dry cal, w=wet cal, p=print cal, r=reset cal, o=valve test");
}

void loop() {
  esp_task_wdt_reset();
  unsigned long now = millis();

  handleSerial();
  maintainWifi();

  if (now - lastWeatherTry >= WEATHER_INTERVAL_MS) updateWeather();

  if (now - lastControlAt >= CONTROL_INTERVAL_MS) {
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
