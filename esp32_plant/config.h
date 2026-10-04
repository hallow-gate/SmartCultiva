#pragma once
#define WIFI_SSID   "your-wifi"
#define WIFI_PASS   "your-password"
#define SERVER_URL  "https://your-app.onrender.com/api/device/sync"
#define DEVICE_KEY  "same-as-DEVICE_API_KEY"
#define TZ_INFO     "PST-8"            // Philippines UTC+8

#define PIN_DHT 4
#define PIN_SOIL 34
#define PIN_RELAY 26
#define RELAY_ON LOW                   // most relay boards are active-LOW
#define RELAY_OFF HIGH
#define PIN_IN1 16
#define PIN_IN2 17
#define PIN_IN3 18
#define PIN_IN4 19

#define SOIL_DRY_RAW 3000              // calibrate: sensor in air
#define SOIL_WET_RAW 1300              // calibrate: sensor in water
#define SOIL_DRY_BELOW_PCT 40          // below this = dry
#define PUMP_MS 30000UL
#define WATER_GAP_MS 300000UL          // min 5 min between manual waterings
#define WINDOW_MIN 15                  // catch-up window after 07:00 / 17:00

#define HOT_C 37.0
#define COOL_C 35.0                    // hysteresis: reopen at/below this
#define CONFIRM_READS 6                // 6 x 5s consecutive readings
#define DHT_MS 5000UL
#define MIN_DWELL_MS 600000UL          // 10 min between auto cover moves
#define MANUAL_HOLD_MS 3600000UL       // manual cover/uncover pauses auto for 1 h
#define SHADE_STEPS 8192L              // 28BYJ-48 half-steps (4096/rev) -> tune
#define SHADE_SPEED 150                // steps/s = slow
#define SYNC_MS 30000UL
