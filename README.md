# ESP32 Plant Watering + Shade System

## Wiring (ESP32 DevKit 30-pin)
| Part | Pin | ESP32 / Supply |
|---|---|---|
| DHT22 | + / OUT / - | 3V3 / GPIO4 (10k pull-up OUT to 3V3) / GND |
| Capacitive soil | VCC / AOUT / GND | 3V3 / GPIO34 / GND |
| DS3231 RTC | VCC / SDA / SCL / GND | 3V3 / GPIO21 / GPIO22 / GND |
| Relay module | VCC / IN / GND | VIN(5V) / GPIO26 / GND |
| ULN2003 (28BYJ-48) | IN1-IN4 | GPIO16, 17, 18, 19 |
| ULN2003 | + / - | external 5V / common GND |

Pump (relay screws, same as your Arduino notes: 1st=NC empty, 2nd=COM, 3rd=NO):
```
5V adapter (+) --+-- ULN2003 (+)
                 +-- Relay COM (2nd screw)
Relay NO (3rd) ----- Pump red (+)
Pump black (-) ----- 5V adapter (-) -- ESP32 GND (COMMON GROUND!)
1N4007 diode across pump: stripe to pump (+)
```
Use a >=2A 5V supply. Never power the pump/stepper from the ESP32 3V3 pin.

## Setup
1. Supabase: run `supabase/schema.sql`; Auth > create your user, disable public sign-ups.
2. Render: Web Service, root dir `server`, build `npm install`, start `npm start`; set env vars from `.env.example`.
3. Firmware: Arduino IDE (ESP32 core 2.x/3.x); install DHT, AccelStepper, RTClib, ArduinoJson 7. Edit `config.h`; calibrate SOIL_DRY_RAW / SOIL_WET_RAW / SHADE_STEPS.

## Safety / idempotency
- Scheduled slot id `water-YYYYMMDD-HH` saved before acting: a reboot can't water twice.
- Every event has a unique `event_id`; DB upsert ignores duplicates; LittleFS outbox retries until the server confirms.
- Web: unique `idem_key` + partial unique index = one open command per action; commands expire after 10 min; device remembers executed command ids.
- Pump hard-stops at 30 s, relay OFF at boot, manual cooldown, soil sensor fault = no watering, hardware watchdog.
- Heat: 6 consecutive readings, 37C close / 35C reopen, 10 min dwell; manual cover/uncover pauses auto for 1 h.
- Render free tier sleeps: first sync may fail, retries in 30 s. No limit switch: add one for homing if power can fail mid-move.
