# Smart Plant Irrigation System (ESP32)

NSS Socially Relevant Project, IIT Tirupati.

An ESP32 firmware that waters plants only when they actually need it. It reads soil moisture from two capacitive probes, checks the rain probability from OpenWeatherMap, and switches a 12 V solenoid valve through a relay.

**Core rule:** irrigate when soil moisture is **below 48.5 %** AND rain probability is **below 40 %**. Stop when moisture reaches **80 %**, when rain is expected, or when a safety limit is hit.

**Team:** Kanishk Navin Patel, Satvik Gaur, Chaman Lal Seju
**Mentor:** Dr B Janaki Ramaiah

---

## How it works

1. **Sense:** averages 16 ADC samples from each of the 2 soil probes and converts each to 0-100 % using its calibration. Faulty probes are ignored.
2. **Decide:** every 2 seconds a state machine (IDLE / WATERING) compares moisture and rain probability with the thresholds.
3. **Water:** the relay opens or closes the solenoid valve.
4. **Verify and log:** a status line is printed to Serial every 10 seconds and on every valve change.

The forecast is fetched every 15 minutes. The highest rain probability over the next ~6 hours is used.

## Hardware

| Part | Notes |
| --- | --- |
| ESP32 DevKit (38-pin) | Controller |
| Capacitive moisture sensor V2.0 x2 | GPIO 32, 33 (ADC1). Two spare probes can be added later on GPIO 34, 35 |
| DHT22 | GPIO 4 (air temperature and humidity, logging only) |
| 1-ch relay module, 5 V, low-level trigger | GPIO 26 |
| 12 V DC solenoid valve | Switched by the relay |
| 12 V 2 A adapter | Powers the valve |
| 1N4007 diode | Across the valve coil (flyback protection) |
| LM2596 buck converter (optional) | Powers the ESP32 from the 12 V supply |

### Wiring

```
Soil probe 1-2 (AOUT) -> GPIO 32 / 33               (VCC 3.3 V, GND)
(Spare probes later: GPIO 34 / 35)
DHT22 data            -> GPIO 4                      (VCC 3.3 V, GND)
Relay IN              -> GPIO 26                     (VCC 5 V, GND)

12 V adapter (+) -> Relay COM
Relay NO         -> Valve (+)
Valve (-)        -> 12 V adapter (-)
1N4007           -> across the valve terminals, stripe (cathode) on the (+) side
All grounds      -> common (ESP32 GND, relay GND, adapter -)
```

Use ADC1 pins (32-39) for the probes. To add the spare probes, change `SOIL_PINS` to `{32, 33, 34, 35}`. Everything else (calibration, averaging, logging) adapts automatically. Redo the dry/wet calibration after adding them. ADC2 pins stop working when WiFi is on.

## Software setup

1. Install the Arduino IDE (or PlatformIO) with the **ESP32 board package**.
2. Install these libraries from the Library Manager:
   - **DHT sensor library** by Adafruit (and **Adafruit Unified Sensor**)
   - **ArduinoJson** by Benoit Blanchon, version **7.x**
3. Keep the sketch in a folder named `smart_irrigation`. The folder name must match the `.ino` file name.
4. Get a free API key from [OpenWeatherMap](https://openweathermap.org/api).
5. Edit the config block at the top of `smart_irrigation.ino`:

   ```cpp
   const char* WIFI_SSID     = "YOUR_WIFI_NAME";
   const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
   const char* OWM_API_KEY   = "YOUR_OPENWEATHERMAP_KEY";
   const char* LATITUDE      = "13.5600";
   const char* LONGITUDE     = "79.5300";
   ```
6. Select your ESP32 board, choose the port, and upload.
7. Open the Serial Monitor at **115200 baud**.

## Calibration (do this before first use)

The default dry and wet values are only typical numbers. Every probe and soil is different.

1. Hold all probes in open air and send `d` in the Serial Monitor.
2. Put the probes in water (up to the line marked on the probe) and send `w`.
3. Send `p` to check the saved values.

Values are saved to flash and survive reboots. For best results, also record raw readings in your target soil before field deployment.

### Serial commands

| Key | Action |
| --- | --- |
| `d` | Save current readings as DRY calibration |
| `w` | Save current readings as WET calibration |
| `p` | Print calibration values |
| `r` | Reset calibration to defaults |
| `o` | Open the valve for 5 s as a test (only when idle) |

## Configuration reference

| Setting | Default | Meaning |
| --- | --- | --- |
| `START_MOISTURE_PCT` | 48.5 | Start watering below this |
| `STOP_MOISTURE_PCT` | 80.0 | Stop watering at or above this |
| `RAIN_LIMIT_PCT` | 40.0 | No watering if rain probability is at or above this |
| `MAX_RUN_MS` | 15 min | Maximum length of one watering cycle |
| `MIN_OFF_MS` | 10 min | Minimum rest between cycles |
| `DAILY_MAX_MS` | 60 min | Maximum watering per 24 h |
| `WEATHER_STALE_MS` | 2 h | Forecast older than this is treated as unavailable |
| `ALLOW_WATERING_WITHOUT_WEATHER` | `false` | If `false`, the valve stays shut without a fresh forecast |
| `FORECAST_ENTRIES` | 2 | Number of 3-hour forecast steps to check |
| `RELAY_ACTIVE_LOW` | `true` | Set to `false` for a high-trigger relay |

## Safety features

- The valve is forced OFF at boot, before anything else runs.
- It closes on a soil sensor fault (all probes invalid). If only one of the two probes fails, the system keeps running on the remaining probe.
- It closes when the forecast becomes stale (unless `ALLOW_WATERING_WITHOUT_WEATHER` is `true`).
- Each cycle has a timeout, there is a rest period between cycles, and there is a daily cap.
- A 30 s hardware watchdog reboots the board if it hangs.
- WiFi reconnects automatically.

## Example output

```
[WEATHER] rain probability = 10 %
[LOG] t=12s soil=41.3% (2/2 probes) air=29.4C hum=61% rain=10% valve=closed daily=0s | IDLE: soil moisture OK
[VALVE] OPEN  - START: soil dry, low rain chance
[LOG] t=14s soil=41.3% (2/2 probes) air=29.4C hum=61% rain=10% valve=OPEN daily=0s | START: soil dry, low rain chance
[VALVE] CLOSE - STOP: upper moisture reached
```

## Testing order

1. Test each probe alone and calibrate.
2. Test the relay with the valve disconnected (listen for the click, use the `o` command).
3. Confirm the weather call works (look for `[WEATHER] rain probability`).
4. Test the full logic with a bucket of water before using real soil.
5. Run a supervised pilot before leaving it unattended.

## Troubleshooting

| Problem | Likely cause and fix |
| --- | --- |
| `soil sensor fault` in the log | A probe is unplugged, or its reading is out of range. Check wiring and 3.3 V supply. |
| Moisture reads 0 % or 100 % always | Calibration is wrong. Redo `d` and `w`. |
| Valve never opens | Check `rain=` in the log (a stale forecast blocks watering), the relay logic level, and the 12 V supply. |
| Relay does the opposite of what is expected | Flip `RELAY_ACTIVE_LOW`. |
| `[WEATHER] HTTP error 401` | Invalid or not yet activated API key (new keys can take a while to activate). |
| Garbage readings with WiFi on | A probe is on an ADC2 pin. Move it to GPIO 32-39. |

## Known limitations

- The weather request skips TLS certificate checking (`setInsecure()`). Add the root CA before any real deployment.
- A floating, unconnected ADC pin may not always be detected as a faulty probe.
- The daily cap resets every 24 hours of uptime, not at midnight.
- The firmware has not been validated on real hardware yet. Outcomes (water saved, yield) should be confirmed with field data in a pilot.

## Future scope

Cloud dashboard and mobile app, AI-based weather and soil prediction, solar-powered version, larger farm automation, and multiple crop support.
