// Libraries: DHT sensor library (Adafruit), AccelStepper, RTClib, ArduinoJson 7
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <DHT.h>
#include <AccelStepper.h>
#include <RTClib.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <esp_task_wdt.h>
#include <vector>
#include "config.h"

const int HOURS[] = {7, 17};
const uint32_t MIN_VALID = 1700000000UL;
DHT dht(PIN_DHT, DHT22);
AccelStepper st(AccelStepper::HALF4WIRE, PIN_IN1, PIN_IN3, PIN_IN2, PIN_IN4);
RTC_DS3231 rtc; bool rtcPresent = false;
Preferences pf;

float tC = NAN, hum = NAN; int soilPct = -1;
bool shadeClosed = false, shadeMoving = false, hold = false;
String shSrc, shId, shCmd; uint32_t lastShade = 0, holdUntil = 0;
bool pumpOn = false, everWatered = false; uint32_t pumpT0 = 0, lastWater = 0; String pSrc, pId, pCmd;
uint32_t lastDht = 0, lastSync = 0, lastSched = 0, lastRtc = 0, lastWifi = 0, boots = 0; int hotN = 0, coolN = 0;

bool timeOk() { return time(nullptr) > MIN_VALID; }
String uid() { return timeOk() ? String((uint32_t)time(nullptr)) : "b" + String(boots) + "-" + String(millis() / 1000); }

// ---------- persistent outbox: events survive reboot/offline, deduped server-side by event_id ----------
void queueEvent(const String& type, const String& src, const String& id, const String& detail, const String& cmd = "") {
  JsonDocument d; d["event_id"] = id; d["type"] = type; d["source"] = src; d["ts"] = (uint32_t)time(nullptr);
  if (cmd.length()) d["command_id"] = cmd;
  d["detail"] = serialized(detail);
  File f = LittleFS.open("/outbox.txt", "a"); serializeJson(d, f); f.println(); f.close();
  lastSync = millis() - SYNC_MS + 2000;                       // sync soon
}
std::vector<String> readOutbox() {
  std::vector<String> v; File f = LittleFS.open("/outbox.txt", "r");
  if (f) { while (f.available()) { String l = f.readStringUntil('\n'); l.trim(); if (l.length()) v.push_back(l); } f.close(); }
  if (v.size() > 100) v.erase(v.begin(), v.end() - 100);
  return v;
}
void writeOutbox(const std::vector<String>& v, size_t from) {
  File f = LittleFS.open("/outbox.txt", "w");
  for (size_t i = from; i < v.size(); i++) f.println(v[i]);
  f.close();
}

// ---------- soil ----------
int soilRaw() { long s = 0; for (int i = 0; i < 16; i++) { s += analogRead(PIN_SOIL); delay(5); } return s / 16; }
int soilPercent(int raw) { return constrain(map(raw, SOIL_DRY_RAW, SOIL_WET_RAW, 0, 100), 0, 100); }

// ---------- pump (non-blocking, hard 30 s cap) ----------
void pumpStart(const String& src, const String& id, const String& cmd) {
  pumpOn = true; pumpT0 = millis(); pSrc = src; pId = id; pCmd = cmd; digitalWrite(PIN_RELAY, RELAY_ON);
}
void pumpStop() {
  digitalWrite(PIN_RELAY, RELAY_OFF); pumpOn = false; lastWater = millis(); everWatered = true;
  queueEvent(pSrc == "schedule" ? "water_auto" : "water_manual", pSrc, pId,
             "{\"result\":\"ok\",\"seconds\":" + String(PUMP_MS / 1000) + ",\"soil_pct\":" + String(soilPct) + "}", pCmd);
}

// ---------- shade (stepper, slow, non-blocking) ----------
void moveShade(bool close, const String& src, const String& id, const String& cmd) {
  shadeMoving = true; shSrc = src; shId = id; shCmd = cmd; shadeClosed = close;   // target; persisted when done
  st.moveTo(close ? SHADE_STEPS : 0);
}
void shadeDone() {
  shadeMoving = false; lastShade = millis(); st.disableOutputs();
  pf.putBool("closed", shadeClosed);
  queueEvent(shadeClosed ? "shade_close" : "shade_open", shSrc, shId,
             "{\"result\":\"ok\",\"temp_c\":" + String(isnan(tC) ? 0 : tC, 1) + "}", shCmd);
}

// ---------- automatic heat logic with hysteresis + confirmation ----------
void heatLogic() {
  if (millis() - lastDht < DHT_MS) return; lastDht = millis();
  float t = dht.readTemperature(), h = dht.readHumidity();
  if (isnan(t) || t < -40 || t > 80) return;                  // ignore bad reads
  tC = t; hum = h;
  if (hold && (int32_t)(millis() - holdUntil) < 0) return;    // manual override active
  hold = false;
  if (t >= HOT_C) { hotN++; coolN = 0; } else if (t <= COOL_C) { coolN++; hotN = 0; } else hotN = coolN = 0;
  bool dwell = lastShade == 0 || millis() - lastShade > MIN_DWELL_MS;
  if (!shadeClosed && hotN >= CONFIRM_READS && dwell) moveShade(true, "auto_heat", "shade-close-" + uid(), "");
  else if (shadeClosed && coolN >= CONFIRM_READS && dwell) moveShade(false, "auto_heat", "shade-open-" + uid(), "");
}

// ---------- scheduled watering 07:00 / 17:00, once per slot (persisted BEFORE acting) ----------
void skip(const String& slot, int raw, const char* why) {
  queueEvent("water_skipped", "schedule", "water-skip-" + slot,
             "{\"result\":\"ok\",\"reason\":\"" + String(why) + "\",\"soil_pct\":" + String(soilPct) + ",\"raw\":" + String(raw) + "}");
}
void scheduleCheck() {
  if (!timeOk() || millis() - lastSched < 10000) return; lastSched = millis();
  time_t n = time(nullptr); struct tm t; localtime_r(&n, &t);
  for (int h : HOURS) {
    if (t.tm_hour != h || t.tm_min >= WINDOW_MIN) continue;
    char slot[16]; snprintf(slot, sizeof slot, "%04d%02d%02d-%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, h);
    if (pf.getString("slot", "") == slot) return;
    pf.putString("slot", slot);
    int raw = soilRaw(); soilPct = soilPercent(raw);
    if (pumpOn) skip(slot, raw, "busy");
    else if (raw < 200 || raw > 4000) skip(slot, raw, "sensor_fault");   // fail safe: never water on faulty sensor
    else if (soilPct >= SOIL_DRY_BELOW_PCT) skip(slot, raw, "wet");
    else pumpStart("schedule", "water-" + String(slot), "");
    return;
  }
}

// ---------- commands from web app (deduped across reboots) ----------
bool seen(const String& id) { return pf.getString("cmds", "").indexOf(id) >= 0; }
void remember(const String& id) {
  String s = pf.getString("cmds", "") + id + ","; if (s.length() > 400) s = s.substring(s.length() - 370); pf.putString("cmds", s);
}
void reject(const char* type, const String& eid, const String& id, const char* why) {
  queueEvent(type, "manual", eid, "{\"result\":\"" + String(why) + "\"}", id);
}
void handleCmd(const String& id, const String& a) {
  if (seen(id)) return; remember(id);
  String eid = "cmd-" + id;
  if (a == "water") {
    if (pumpOn) return reject("water_manual", eid, id, "busy");
    if (everWatered && millis() - lastWater < WATER_GAP_MS) return reject("water_manual", eid, id, "cooldown");
    soilPct = soilPercent(soilRaw()); pumpStart("manual", eid, id);
  } else if (a == "cover" || a == "uncover") {
    bool want = a == "cover"; const char* ty = want ? "shade_close" : "shade_open";
    if (shadeMoving) return reject(ty, eid, id, "busy");
    hold = true; holdUntil = millis() + MANUAL_HOLD_MS;
    if (shadeClosed == want) queueEvent(ty, "manual", eid, "{\"result\":\"ok\",\"noop\":true}", id);
    else moveShade(want, "manual", eid, id);
  }
}

// ---------- server sync (events out, commands in) ----------
void syncServer() {
  if (WiFi.status() != WL_CONNECTED) return;
  soilPct = soilPercent(soilRaw());
  auto lines = readOutbox(); size_t n = min(lines.size(), (size_t)10);
  JsonDocument req; JsonObject t = req["telemetry"].to<JsonObject>();
  if (!isnan(tC)) { t["temp_c"] = tC; t["humidity"] = hum; }
  t["soil_pct"] = soilPct; t["shade"] = shadeClosed ? "closed" : "open"; t["pump"] = pumpOn;
  JsonArray ev = req["events"].to<JsonArray>();
  for (size_t i = 0; i < n; i++) { JsonDocument e; if (!deserializeJson(e, lines[i])) ev.add(e); }  // corrupt line => dropped
  String body; serializeJson(req, body);
  WiFiClientSecure c; c.setInsecure();                      // for production pin Render's root CA instead
  HTTPClient http; http.setTimeout(15000); http.begin(c, SERVER_URL);
  http.addHeader("Content-Type", "application/json"); http.addHeader("x-device-key", DEVICE_KEY);
  int code = http.POST(body);
  if (code == 200) {
    JsonDocument res;
    if (!deserializeJson(res, http.getString())) {
      writeOutbox(lines, n);                                // only drop after server confirmed
      for (JsonObject cmd : res["commands"].as<JsonArray>()) handleCmd(cmd["id"].as<String>(), cmd["action"].as<String>());
    }
  }
  http.end();
}

void setup() {
  pinMode(PIN_RELAY, OUTPUT); digitalWrite(PIN_RELAY, RELAY_OFF);   // pump OFF first
  Serial.begin(115200); LittleFS.begin(true);
  pf.begin("plant", false); boots = pf.getUInt("boots", 0) + 1; pf.putUInt("boots", boots);
  analogSetPinAttenuation(PIN_SOIL, ADC_11db); dht.begin();
  shadeClosed = pf.getBool("closed", false);
  st.setMaxSpeed(SHADE_SPEED); st.setAcceleration(100); st.setCurrentPosition(shadeClosed ? SHADE_STEPS : 0); st.disableOutputs();
  Wire.begin(21, 22); rtcPresent = rtc.begin();
  WiFi.mode(WIFI_STA); WiFi.setAutoReconnect(true); WiFi.begin(WIFI_SSID, WIFI_PASS);
  configTzTime(TZ_INFO, "pool.ntp.org", "time.google.com");
  uint32_t t0 = millis(); while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) delay(250);
  struct tm tm; bool ntp = WiFi.status() == WL_CONNECTED && getLocalTime(&tm, 8000);
  if (ntp && rtcPresent) rtc.adjust(DateTime((uint32_t)time(nullptr)));
  else if (!ntp && rtcPresent && !rtc.lostPower()) { struct timeval tv = {(time_t)rtc.now().unixtime(), 0}; settimeofday(&tv, nullptr); }
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  esp_task_wdt_config_t wc = {.timeout_ms = 45000, .idle_core_mask = 0, .trigger_panic = true}; esp_task_wdt_reconfigure(&wc);
#else
  esp_task_wdt_init(45, true);
#endif
  esp_task_wdt_add(NULL);
}

void loop() {
  esp_task_wdt_reset();
  st.run();
  if (pumpOn && millis() - pumpT0 >= PUMP_MS) pumpStop();
  if (shadeMoving && st.distanceToGo() == 0) shadeDone();
  if (!shadeMoving) heatLogic();
  scheduleCheck();
  if (!pumpOn && !shadeMoving && millis() - lastSync >= SYNC_MS) { lastSync = millis(); syncServer(); }
  if (WiFi.status() != WL_CONNECTED && millis() - lastWifi > 30000) { lastWifi = millis(); WiFi.reconnect(); }
  if (rtcPresent && timeOk() && WiFi.status() == WL_CONNECTED && millis() - lastRtc > 21600000UL) { lastRtc = millis(); rtc.adjust(DateTime((uint32_t)time(nullptr))); }
}
