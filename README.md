# Smart Plant Irrigation System (ESP32)

NSS Socially Relevant Project, IIT Tirupati.

An ESP32-based irrigation controller that waters plants only when they actually need it. It reads soil moisture from capacitive probes and switches a 12 V solenoid valve through a relay. This repository has **two firmware versions**:

| Version | Folder | Uses | Best for |
| --- | --- | --- | --- |
| **Weather-aware** | `smart_irrigation/` | Soil moisture + OpenWeatherMap rain forecast + DHT22 logging | Final system, saves water by skipping watering when rain is coming |
| **Soil-only** | `smart_irrigation_soil_only/` | Soil moisture only, no WiFi | Bench testing, first prototype, places without WiFi |

**Team:** Kanishk Navin Patel, Satvik Gaur, Chaman Lal Seju
**Mentor:** Dr B Janaki Ramaiah

---

## Repository structure

```
smart-irrigation-esp32/
├── README.md
├── smart_irrigation/
│   └── smart_irrigation.ino              # with weather API
└── smart_irrigation_soil_only/
    └── smart_irrigation_soil_only.ino    # soil moisture only
```

Each sketch must stay in a folder with the same name as its `.ino` file, or the Arduino IDE will refuse to open it.

## Which version should I use?

- Start with **soil-only** to check wiring, calibration and the valve. It needs no WiFi, no API key and no extra libraries.
- Move to the **weather-aware** version once the hardware works. It adds the rain check, so the system does not water when rain is expected.
- If WiFi or the forecast is unavailable, the weather-aware version fails safe (valve stays shut) by default. If you would rather it keep watering on soil data alone, set `ALLOW_WATERING_WITHOUT_WEATHER` to `true`, or use the soil-only version.

## How it works

### Soil-only version
1. **Sense:** averages 16 ADC samples from each soil probe and converts each to 0-100 % using its calibration. Faulty probes are ignored.
2. **Decide:** every 2 seconds a state machine compares moisture with the thresholds.
3. **Water:** the relay opens or closes the valve.
4. **Verify and log:** a status line is printed to Serial every 10 seconds and on every valve change.

**Rule:** water when soil moisture is **below 48.5 %**, stop at **80 %**.

### Weather-aware version
Same as above, plus:
- Every 15 minutes (and at startup) it fetches the OpenWeatherMap forecast and takes the highest rain probability over the next ~6 hours.
- It also logs air temperature and humidity from the DHT22 (logging only, not used for decisions).

**Rule:** water when soil moisture is **below 48.5 %** AND rain probability is **below 40 %**. Stop at **80 %** moisture, or when rain probability reaches 40 %.

### Decision logic summary

| | Soil-only | Weather-aware |
| --- | --- | --- |
| Start watering | Moisture < 48.5 % | Moisture < 48.5 % **and** rain < 40 % |
| Stop: upper moisture | >= 80 % | >= 80 % |
| Stop: rain | Not checked | Rain >= 40 % |
| Stop: forecast stale (> 2 h) | Not applicable | Yes (unless `ALLOW_WATERING_WITHOUT_WEATHER`) |
| Stop: safety timeout | 15 min per cycle | 15 min per cycle |
| Stop: daily cap | 60 min per 24 h | 60 min per 24 h |
| Stop: all probes faulty | Yes | Yes |
| Rest between cycles | 10 min | 10 min |

## Hardware

| Part | Notes |
| --- | --- |
| ESP32-WROOM-32D DevKit (38-pin) | Controller. Select **ESP32 Dev Module** in the Arduino IDE |
| Capacitive moisture sensor V2.0 x2 | GPIO 32, 33 (ADC1). Two spares can be added later on GPIO 34, 35 |
| 1-ch relay module, 5 V, low-level trigger | GPIO 26 |
| 12 V DC solenoid valve | Switched by the relay |
| 12 V 2 A adapter | Powers the valve |
| 1N4007 diode | Across the valve coil (flyback protection) |
| DHT22 (weather-aware version only) | GPIO 4, logging only |
| LM2596 buck converter (optional) | Powers the ESP32 from the 12 V supply |

### Wiring

```
Soil probe 1-2 (AOUT) -> GPIO 32 / 33               (VCC 3.3 V, GND)
(Spare probes later: GPIO 34 / 35)
Relay IN              -> GPIO 26                     (VCC 5 V, GND)
DHT22 data            -> GPIO 4                      (weather-aware only; VCC 3.3 V, GND)

12 V adapter (+) -> Relay COM
Relay NO         -> Valve (+)
Valve (-)        -> 12 V adapter (-)
1N4007           -> across the valve terminals, stripe (cathode) on the (+) side
All grounds      -> common (ESP32 GND, relay GND, adapter -)
```

Use ADC1 pins (32-39) for the probes. ADC2 pins stop working when WiFi is on. To add the spare probes, change `SOIL_PINS` to `{32, 33, 34, 35}`. Everything else adapts automatically. Redo the calibration afterwards.

## Software setup

### Both versions
1. Install the Arduino IDE (or PlatformIO) with the **ESP32 board package** (esp32 by Espressif Systems).
2. Select **ESP32 Dev Module**, choose the port, and set the Serial Monitor to **115200 baud**.
3. If upload shows "Connecting......", hold the **BOOT** button on the board until it starts.

### Soil-only version
- No extra libraries needed.
- Open `smart_irrigation_soil_only/smart_irrigation_soil_only.ino` and upload.

### Weather-aware version
1. Install these libraries from the Library Manager:
   - **DHT sensor library** by Adafruit (and **Adafruit Unified Sensor**)
   - **ArduinoJson** by Benoit Blanchon, version **7.x**
2. Get a free API key from [OpenWeatherMap](https://openweathermap.org/api).
3. Edit the config block at the top of `smart_irrigation.ino`:

   ```cpp
   const char* WIFI_SSID     = "YOUR_WIFI_NAME";
   const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
   const char* OWM_API_KEY   = "YOUR_OPENWEATHERMAP_KEY";
   const char* LATITUDE      = "13.5600";
   const char* LONGITUDE     = "79.5300";
   ```
4. Upload. The ESP32 supports **2.4 GHz WiFi only**. Campus or hostel networks with a login page (captive portal) will not work, so use a phone hotspot to test.

**Never commit your real WiFi password or API key.** Keep placeholders in the repo. If a key is ever pushed by mistake, regenerate it, because deleting it in a later commit does not remove it from git history.

## Calibration (do this before first use, both versions)

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

### Both versions

| Setting | Default | Meaning |
| --- | --- | --- |
| `SOIL_PINS` | `{32, 33}` | Active soil probe pins |
| `RELAY_PIN` | 26 | Relay control pin |
| `RELAY_ACTIVE_LOW` | `true` | Set to `false` for a high-trigger relay |
| `START_MOISTURE_PCT` | 48.5 | Start watering below this |
| `STOP_MOISTURE_PCT` | 80.0 | Stop watering at or above this |
| `MAX_RUN_MS` | 15 min | Maximum length of one watering cycle |
| `MIN_OFF_MS` | 10 min | Minimum rest between cycles |
| `DAILY_MAX_MS` | 60 min | Maximum watering per 24 h |

### Weather-aware version only

| Setting | Default | Meaning |
| --- | --- | --- |
| `RAIN_LIMIT_PCT` | 40.0 | No watering if rain probability is at or above this |
| `WEATHER_STALE_MS` | 2 h | Forecast older than this counts as unavailable |
| `ALLOW_WATERING_WITHOUT_WEATHER` | `false` | If `false`, the valve stays shut without a fresh forecast |
| `FORECAST_ENTRIES` | 2 | Number of 3-hour forecast steps to check |
| `WEATHER_INTERVAL_MS` | 15 min | How often the forecast is fetched |

## Safety features (both versions)

- The valve is forced OFF at boot, before anything else runs.
- It closes on a soil sensor fault (all probes invalid). If only one of two probes fails, it keeps running on the other.
- Each cycle has a timeout, there is a rest period between cycles, and there is a daily cap.
- A 30 s hardware watchdog reboots the board if it hangs.

Weather-aware only: WiFi reconnects automatically, and the valve closes if the forecast goes stale (unless `ALLOW_WATERING_WITHOUT_WEATHER` is `true`).

## Example output

Soil-only:

```
[LOG] t=12s soil=41.3% (2/2 probes) valve=closed daily=0s | IDLE: waiting min off-time
[VALVE] OPEN  - START: soil dry
[VALVE] CLOSE - STOP: upper moisture reached
```

Weather-aware:

```
[WEATHER] rain probability = 10 %
[LOG] t=12s soil=41.3% (2/2 probes) air=29.4C hum=61% rain=10% valve=closed daily=0s | IDLE: soil moisture OK
[VALVE] OPEN  - START: soil dry, low rain chance
[VALVE] CLOSE - STOP: rain expected
```

## Testing order

1. Test each probe alone and calibrate.
2. Test the relay with the valve disconnected (listen for the click, use the `o` command).
3. Test the full logic with a bucket of water before using real soil (soil-only version first).
4. Weather-aware only: confirm the forecast call works by looking for `[WEATHER] rain probability` in the Serial Monitor.
5. Run a supervised pilot before leaving it unattended.

## Troubleshooting

| Problem | Likely cause and fix |
| --- | --- |
| `soil sensor fault` in the log | A probe is unplugged or its reading is out of range. Check wiring and the 3.3 V supply. |
| Moisture reads 0 % or 100 % always | Calibration is wrong. Redo `d` and `w`. |
| Relay does the opposite of what is expected | Flip `RELAY_ACTIVE_LOW`. |
| Valve never opens (weather-aware) | Check `rain=` in the log. A stale forecast blocks watering. |
| `[WEATHER] HTTP error 401` | Invalid or not yet activated API key. New keys can take a while to activate. |
| No WiFi connection | The ESP32 only supports 2.4 GHz. Captive-portal networks will not work. |
| Garbage readings with WiFi on | A probe is on an ADC2 pin. Move it to GPIO 32-39. |
| Sketch will not open in Arduino IDE | The folder name must match the `.ino` file name. |

## Known limitations

- The firmware has not been validated on real hardware yet. Outcomes (water saved, yield) should be confirmed with field data in a pilot.
- The soil-only version does not know about rain and will water even if rain is coming.
- The weather request skips TLS certificate checking (`setInsecure()`). Add the root CA before any real deployment.
- A floating, unconnected ADC pin may not always be detected as a faulty probe.
- The daily cap resets every 24 hours of uptime, not at midnight.

## Future scope

Cloud dashboard and mobile app, AI-based weather and soil prediction, solar-powered version, larger farm automation, and multiple crop support.
