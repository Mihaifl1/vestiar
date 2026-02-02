/* esp_vestiar_complete.ino
   ESP8266 NodeMCU — Vestiar complet:
   - Wiegand (keypad + RFID)
   - Relay control
   - MQTT (cmd/status/events, cards list, schedule)
   - LittleFS persistence
   - NTP time (în UI & serial)
   - Web UI (programări + carduri) cu CORS
   - Status MQTT include: ip, mac, rssi, door, pwr
   - Publică și topicul info: locker/<ID>/info (ip, freeHeap)
   - UI preia din PHP (vestiar_dbinfo.php) "ultimul card din DB" + numărători
*/

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <time.h>
#include <vector>
#include <ESP8266HTTPClient.h>   // ✅ adaugă asta
#include <WiFiClient.h>          // ✅ și asta
// ---------- CONFIG ----------
const char* WIFI_SSID = "vestiar";
const char* WIFI_PASS = "12345678";

const char* MQTT_HOST = "broker.emqx.io";
const uint16_t MQTT_PORT = 1883;

const int DOOR_ID = 7;

// Relay
const int PIN_RELAY = 16; // D0
const bool RELAY_ACTIVE_LOW = true;
const unsigned long RELAY_PULSE_MS = 0; // 0 = hold (fără puls)
bool relayAutoLockActive = false;
unsigned long lastRelayOn = 0;
// Wiegand pins
const int PIN_KPD_D0  = 14; // D5
const int PIN_KPD_D1  = 12; // D6
const int PIN_RFID_D0 = 13; // D7
const int PIN_RFID_D1 = 15; // D8  (boot: trebuie LOW)

// Enroll button
const int PIN_ENROLL = 5; // D1

// Optional sensors
const int PIN_DOOR  = 4;  // D2 (reed) LOW=CLOSED
const int PIN_PWR   = 0;  // D3 (power) HIGH=ON

// Files
const char* FILE_CARDS    = "/cards.json";
const char* FILE_SCHEDULE = "/schedule.json";
const char* FILE_EVENTS   = "/events.log";
const size_t EVENTS_FILE_MAX = 60000;

// Time / NTP
const long TZ_OFFSET  = 2 * 3600;
const long DST_OFFSET = 1 * 3600;
const char* NTP1 = "pool.ntp.org";
const char* NTP2 = "time.nist.gov";

// Schedule limits
#define MAX_RULES 32

// ---------- GLOBALS ----------
WiFiClient wifi;
PubSubClient mqtt(wifi);
ESP8266WebServer server(80);

String T_CMD, T_STATUS, T_EVENT;
String T_CARDS_ADD, T_CARDS_REMOVE, T_CARDS_LIST_REQ, T_CARDS_LIST_RESP;
String T_SCHED_SET, T_SCHED_GET, T_SCHED_CUR;

String lockState = "LOCKED"; // LOCKED | UNLOCKED

struct CardEntry { unsigned long card; String first; String last; };
std::vector<CardEntry> allowedCards;

struct Rule {
  bool days[7];         // 0..6 = Mon..Sun (Monday=0)
  uint16_t minuteOfDay; // HH*60 + MM
  String action;        // "UNLOCK" or "LOCK"
  String note;
};
Rule rules[MAX_RULES];
int rulesCount = 0;

// Wiegand
volatile unsigned long long wgData = 0;
volatile uint8_t wgBits = 0;
volatile unsigned long lastPulseMicros = 0;
const unsigned long endTimeout = 30000UL; // 30 ms
int currentSource = 0; // 1=keypad, 2=rfid

// Enroll
volatile bool enrollRequest = false;
bool enrollMode = false;
unsigned long enrollTs = 0;
const unsigned long ENROLL_TIMEOUT = 15000UL;

// Keypad buffer & master
String keypadBuffer = "";
String masterCode = "12369";

// Duplicate suppression
unsigned long lastWgTs = 0;
unsigned long long lastWgDataFull = 0;
uint8_t lastWgBits = 0;

// Print time
unsigned long lastSecondPrint = 0;

// ---------- UTIL ----------
String timeToStr(time_t ts) {
  struct tm t; localtime_r(&ts, &t);
  char buf[32]; strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &t);
  return String(buf);
}
String nowStamp() {
  time_t n = time(nullptr);
  if (n < 100000) return String("NTP not ready");
  return timeToStr(n);
}
int dowMonday0(const tm& t) { int w = t.tm_wday; return (w == 0) ? 6 : (w - 1); }
void printMemoryInfo() {
  Serial.printf("MEM freeHeap=%u\n", (unsigned)ESP.getFreeHeap());
  if (LittleFS.exists(FILE_CARDS))    { File f = LittleFS.open(FILE_CARDS, "r");    if (f) { Serial.printf("FS %s size=%u\n", FILE_CARDS, (unsigned)f.size()); f.close(); } }
  if (LittleFS.exists(FILE_SCHEDULE)) { File f = LittleFS.open(FILE_SCHEDULE, "r"); if (f) { Serial.printf("FS %s size=%u\n", FILE_SCHEDULE, (unsigned)f.size()); f.close(); } }
}
String ipStr() {
  if (WiFi.status() == WL_CONNECTED) return WiFi.localIP().toString();
  return String("-");
}

// ---------- FILES: CARDS ----------
bool loadCardsFromFS() {
  allowedCards.clear();
  if (!LittleFS.exists(FILE_CARDS)) return true;
  File f = LittleFS.open(FILE_CARDS, "r"); if (!f) return false;
  size_t sz = f.size(); size_t cap = sz + 2048; if (cap > 32768) cap = 32768;
  DynamicJsonDocument doc(cap);
  DeserializationError err = deserializeJson(doc, f); f.close();
  if (err) { Serial.printf("cards.json parse err: %s\n", err.c_str()); return false; }
  JsonArray arr = doc.as<JsonArray>();
  if (arr.isNull()) return false;
  for (JsonVariant v : arr) {
    JsonObject o = v.as<JsonObject>();
    if (!o.isNull()) {
      CardEntry e; e.card = (unsigned long)(o["card"] | 0UL);
      e.first = String((const char*)(o["first"] | "")); e.last = String((const char*)(o["last"] | ""));
      if (e.card) allowedCards.push_back(e);
    } else {
      unsigned long long num = v.as<unsigned long long>();
      if (num) { CardEntry e; e.card = (unsigned long)num; e.first = ""; e.last = ""; allowedCards.push_back(e); }
    }
  }
  Serial.printf("Loaded %u cards from FS\n", (unsigned)allowedCards.size());
  return true;
}

bool saveCardsToFS() {
  size_t cap = 512 + allowedCards.size() * 128; if (cap > 32768) cap = 32768;
  DynamicJsonDocument doc(cap);
  JsonArray arr = doc.to<JsonArray>();
  for (auto &c : allowedCards) {
    JsonObject o = arr.createNestedObject();
    o["card"] = c.card;
    if (c.first.length()) o["first"] = c.first;
    if (c.last.length())  o["last"]  = c.last;
  }
  File f = LittleFS.open(FILE_CARDS, "w"); if (!f) return false;
  if (serializeJson(doc, f) == 0) { f.close(); return false; }
  f.close();
  return true;
}

// ---------- SCHEDULE ----------
bool parseScheduleJson(const String& json) {
  Serial.printf("SCHED parse request len=%u freeHeap=%u\n", (unsigned)json.length(), (unsigned)ESP.getFreeHeap());
  size_t cap = (size_t)(json.length() * 1.2) + 1024; if (cap < 2048) cap = 2048; if (cap > 32768) cap = 32768;
  DynamicJsonDocument doc(cap);
  DeserializationError err = deserializeJson(doc, json);
  if (err) { Serial.printf("parseScheduleJson err: %s\n", err.c_str()); return false; }
  JsonArray arr = doc["rules"].as<JsonArray>();
  if (arr.isNull()) { Serial.println("parseScheduleJson: rules missing or invalid"); return false; }
  rulesCount = 0;
  for (int i = 0; i < MAX_RULES; ++i) for (int d = 0; d < 7; ++d) rules[i].days[d] = false;
  for (size_t i = 0; i < arr.size() && rulesCount < MAX_RULES; ++i) {
    JsonObject r = arr[i].as<JsonObject>(); if (r.isNull()) continue;
    Rule &R = rules[rulesCount];
    for (int d = 0; d < 7; ++d) R.days[d] = false;
    if (r.containsKey("days")) {
      JsonArray ds = r["days"].as<JsonArray>();
      if (!ds.isNull()) for (JsonVariant dv : ds) { int vi = dv.as<int>(); if (vi >= 0 && vi <= 6) R.days[vi] = true; }
    } else {
      for (int d = 0; d < 5; ++d) R.days[d] = true; // Mon-Fri implicit
    }
    const char* tt = r["time"] | "08:00";
    int H = 8, M = 0; sscanf(tt, "%d:%d", &H, &M);
    if (H < 0) H = 0; if (H > 23) H = H % 24; if (M < 0) M = 0; if (M > 59) M = M % 60;
    R.minuteOfDay = (uint16_t)(H * 60 + M);
    R.action = String((const char*)(r["action"] | "UNLOCK")); R.action.toUpperCase();
    R.note = String((const char*)(r["note"] | ""));
    Serial.printf(" parsed rule[%u]: time=%02d:%02d action=%s note='%s'\n",
                  (unsigned)rulesCount, H, M, R.action.c_str(), R.note.c_str());
    rulesCount++;
  }
  Serial.printf("[%s] Schedule loaded: %d rules\n", nowStamp().c_str(), rulesCount);
  return true;
}

bool loadScheduleFS() {
  if (!LittleFS.exists(FILE_SCHEDULE)) return false;
  File f = LittleFS.open(FILE_SCHEDULE, "r"); if (!f) return false;
  String s = f.readString(); f.close();
  return parseScheduleJson(s);
}

bool saveScheduleFS(const String& json) {
  File f = LittleFS.open(FILE_SCHEDULE, "w"); if (!f) return false;
  f.print(json); f.close(); return true;
}

String scheduleAsJson() {
  size_t cap = 1024 + rulesCount * 128; if (cap > 32768) cap = 32768;
  DynamicJsonDocument doc(cap);
  JsonArray arr = doc.createNestedArray("rules");
  for (int i = 0; i < rulesCount; ++i) {
    JsonObject o = arr.createNestedObject();
    JsonArray ds = o.createNestedArray("days");
    for (int d = 0; d < 7; ++d) if (rules[i].days[d]) ds.add(d);
    char buf[6]; sprintf(buf, "%02d:%02d", rules[i].minuteOfDay / 60, rules[i].minuteOfDay % 60);
    o["time"] = buf; o["action"] = rules[i].action; if (rules[i].note.length()) o["note"] = rules[i].note;
  }
  String out; serializeJson(doc, out); return out;
}

// ---------- EVENTS ----------
void ensureEventsFileSize() {
  if (!LittleFS.exists(FILE_EVENTS)) return;
  File f = LittleFS.open(FILE_EVENTS, "r"); if (!f) return;
  size_t sz = f.size(); f.close();
  if (sz > EVENTS_FILE_MAX) {
    File fw = LittleFS.open(FILE_EVENTS, "w"); if (fw) { fw.println(String("[TRUNCATED at ") + nowStamp() + "]"); fw.close(); }
    Serial.println("Events log truncated");
  }
}
void publishEventJson(const JsonObject &obj) {
  String out; serializeJson(obj, out);
  if (T_EVENT.length()) mqtt.publish(T_EVENT.c_str(), out.c_str(), false);
  ensureEventsFileSize();
  File f = LittleFS.open(FILE_EVENTS, "a"); if (f) { f.println(out); f.close(); }
}
void logEvent(const char* type, const char* source, const char* extra = nullptr) {
  DynamicJsonDocument doc(512); doc["ts"] = nowStamp(); doc["type"] = type; doc["source"] = source; if (extra) doc["extra"] = extra;
  publishEventJson(doc.as<JsonObject>());
}
void logCardEvent(unsigned long card24, unsigned int facility, unsigned int cardNumber, bool ok) {
  DynamicJsonDocument doc(512);
  doc["ts"] = nowStamp(); doc["type"] = "CARD"; doc["ok"] = ok; doc["card24"] = card24;
  doc["facility"] = facility; doc["cardNumber"] = cardNumber;
  int idx = -1; for (size_t i = 0; i < allowedCards.size(); ++i) if (allowedCards[i].card == card24) { idx = (int)i; break; }
  if (idx >= 0) { if (allowedCards[idx].first.length()) doc["first"] = allowedCards[idx].first; if (allowedCards[idx].last.length()) doc["last"] = allowedCards[idx].last; }
  publishEventJson(doc.as<JsonObject>());
}
void logKeypadEvent(const String &code, bool ok) {
  DynamicJsonDocument doc(256); doc["ts"] = nowStamp(); doc["type"] = "KEYPAD"; doc["ok"] = ok; doc["code"] = code; publishEventJson(doc.as<JsonObject>());
}

// ---------- RELAY ----------
void setRelayState(bool on, bool byCard = false, bool autoLock = false) {
  static unsigned long relayOnSince = 0;
  static bool waitingAutoLock = false;

  int level = RELAY_ACTIVE_LOW ? (on ? LOW : HIGH) : (on ? HIGH : LOW);
  digitalWrite(PIN_RELAY, level);
  lockState = on ? "UNLOCKED" : "LOCKED";

  Serial.printf("Relay %s (level=%d) byCard=%d autoLock=%d\n",
                on ? "ON" : "OFF", level, byCard ? 1 : 0, autoLock ? 1 : 0);

  if (on && autoLock) {            // card sau tastatură
    relayOnSince = millis();
    waitingAutoLock = true;
  } else {
    waitingAutoLock = false;
  }

  // salvăm starea globală pentru verificare în loop()
  if (on) relayOnSince = millis();
  else waitingAutoLock = false;

  // reținem intern starea de autoLock
  if (autoLock) lockState = "AUTOLOCK";
  else lockState = on ? "UNLOCKED" : "LOCKED";

  // memorează în variabile globale pentru loop()
  lastRelayOn = relayOnSince;
  relayAutoLockActive = waitingAutoLock;
}


void publishStatus() {
  DynamicJsonDocument d(256);
  d["lock"] = lockState;
  d["ts"]   = nowStamp();
  d["ip"]   = ipStr();
  d["mac"]  = WiFi.macAddress();
  d["rssi"] = (int)WiFi.RSSI();
  String door = (digitalRead(PIN_DOOR) == LOW) ? "CLOSED" : "OPEN";
  String pwr  = (digitalRead(PIN_PWR)  == HIGH) ? "ON"     : "OFF";
  d["door"] = door;
  d["pwr"]  = pwr;
  String out; serializeJson(d, out);
  if (T_STATUS.length()) mqtt.publish(T_STATUS.c_str(), out.c_str(), true); // retained
}
void publishInfo() {
  DynamicJsonDocument d(192);
  d["ip"] = ipStr();
  d["freeHeap"] = (unsigned)ESP.getFreeHeap();
  String out; serializeJson(d, out);
  String topic = "locker/" + String(DOOR_ID) + "/info";
  mqtt.publish(topic.c_str(), out.c_str(), true); // retained
}

// ---------- WIEGAND ----------
void ICACHE_RAM_ATTR handleKPD_D0() { wgData = (wgData << 1); wgBits++; lastPulseMicros = micros(); currentSource = 1; }
void ICACHE_RAM_ATTR handleKPD_D1() { wgData = (wgData << 1) | 1ULL; wgBits++; lastPulseMicros = micros(); currentSource = 1; }
void ICACHE_RAM_ATTR handleRFID_D0() { wgData = (wgData << 1); wgBits++; lastPulseMicros = micros(); currentSource = 2; }
void ICACHE_RAM_ATTR handleRFID_D1() { wgData = (wgData << 1) | 1ULL; wgBits++; lastPulseMicros = micros(); currentSource = 2; }

char mapKeyNormalized(unsigned int normVal) {
  switch (normVal) {
    case 30: case 93: return '1';
    case 45: case 178: return '2';
    case 60: case 209: return '3';
    case 75: return '4';
    case 90: return '5';
    case 105: case 419: return '6';
    case 120: return '7';
    case 135: return '8';
    case 150: return '9';
    case 15: return '0';
    case 165: return '*';
    case 180: case 756: return '#';
    default: return '?';
  }
}
char mapKeyCode(unsigned long long data, uint8_t bits) {
  if (bits <= 9) { unsigned int v = (unsigned int)(data & 0x1FFULL); return mapKeyNormalized(v); }
  unsigned int low8 = (unsigned int)(data & 0xFFULL);
  char c = mapKeyNormalized(low8); if (c != '?') return c;
  unsigned int low9 = (unsigned int)(data & 0x1FFULL); c = mapKeyNormalized(low9); if (c != '?') return c;
  if (bits > 8) { unsigned long long reduced = data >> (bits - 8); return mapKeyNormalized((unsigned int)(reduced & 0xFFULL)); }
  return '?';
}

void processWiegand() {
  noInterrupts();
  uint8_t bits = wgBits;
  unsigned long long data = wgData;
  unsigned long last = lastPulseMicros;
  int src = currentSource;
  interrupts();

  if (bits == 0) return;
  if ((micros() - last) <= endTimeout) return;

  noInterrupts();
  wgBits = 0; wgData = 0; currentSource = 0;
  interrupts();

  Serial.printf("SRC=%d Wg bits=%u data=%llu\n", src, bits, data);

  unsigned long nowMs = millis();
  if (data == lastWgDataFull && bits == lastWgBits && (nowMs - lastWgTs) < 300) { lastWgTs = nowMs; Serial.println("Duplicate Wg ignored"); return; }
  lastWgDataFull = data; lastWgBits = bits; lastWgTs = nowMs;

  if (bits < 7) { Serial.printf("Ignoring small packet (%u)\n", bits); return; }

  bool treatAsCard = (bits >= 24) || (src == 2);
  if (treatAsCard) {
    unsigned long card24 = 0;
    if (bits == 26) card24 = (unsigned long)((data >> 1) & 0x00FFFFFFULL);
    else if (bits == 24) card24 = (unsigned long)(data & 0x00FFFFFFULL);
    else if (bits == 34) { unsigned long long payload32 = (data >> 1) & 0xFFFFFFFFULL; card24 = (unsigned long)(payload32 & 0x00FFFFFFUL); }
    else { if (bits > 24) card24 = (unsigned long)((data >> (bits - 24)) & 0x00FFFFFFULL); else card24 = (unsigned long)(data & 0x00FFFFFFULL); }
    unsigned int facility = (unsigned int)((card24 >> 16) & 0xFF);
    unsigned int cardNum = (unsigned int)(card24 & 0xFFFF);
    bool ok = false; for (auto &c : allowedCards) if (c.card == card24) { ok = true; break; }
    logCardEvent(card24, facility, cardNum, ok);
    sendEventToServer(card24, facility, cardNum, ok);

    Serial.printf("CARD: %lu fac=%u num=%u ok=%d\n", card24, facility, cardNum, ok ? 1 : 0);
    if (enrollMode) {
      DynamicJsonDocument d(256); d["card"] = card24; d["first"] = ""; d["last"] = "";
      String out; serializeJson(d, out); if (T_CARDS_ADD.length()) mqtt.publish(T_CARDS_ADD.c_str(), out.c_str(), false);
      enrollMode = false; logEvent("ENROLL", "CARD_SENT");
    } else if (ok) { setRelayState(true, true, true); publishStatus(); }
    return;
  }

  char ch = mapKeyCode(data, bits);
  if (ch != '?') {
    if (ch == '#') {
      bool ok = (keypadBuffer == masterCode);
      logKeypadEvent(keypadBuffer, ok);
      if (ok) { setRelayState(true, true, true); publishStatus(); }
      keypadBuffer = "";
      return;
    } else if (ch == '*') { keypadBuffer = ""; return; }
    else {
      static unsigned long lastKeyMs = 0;
      if (millis() - lastKeyMs > 60) { keypadBuffer += ch; lastKeyMs = millis(); Serial.printf("Key %c buffer=%s\n", ch, keypadBuffer.c_str()); }
      else Serial.printf("Key repeat ignored %c\n", ch);
      return;
    }
  }

  Serial.printf("Unknown keypad data=%llu bits=%u\n", data, bits);
  DynamicJsonDocument ud(192); ud["ts"] = nowStamp(); ud["type"] = "WG_UNKNOWN"; ud["src"] = src; ud["bits"] = bits; ud["data"] = String((unsigned long long)data);
  publishEventJson(ud.as<JsonObject>());
}

// ---------- SCHEDULE EVALUATION ----------
void evaluateScheduleEachMinute() {
  time_t now = time(nullptr); if (now < 100000) return;
  static int lastMin = -1;
  tm t; localtime_r(&now, &t);
  if (t.tm_min == lastMin) return;
  lastMin = t.tm_min;
  int day = dowMonday0(t);
  uint16_t curMin = (uint16_t)(t.tm_hour * 60 + t.tm_min);
  for (int i = 0; i < rulesCount; ++i) {
    if (!rules[i].days[day]) continue;
    if (rules[i].minuteOfDay == curMin) {
      Serial.printf("[%s] SCHEDULE match %02d:%02d -> %s\n", nowStamp().c_str(), curMin / 60, curMin % 60, rules[i].action.c_str());
      if (rules[i].action == "UNLOCK") { setRelayState(true, false); publishStatus(); logEvent("SCHEDULE", "UNLOCK", rules[i].note.c_str()); }
      else if (rules[i].action == "LOCK") { setRelayState(false, false); publishStatus(); logEvent("SCHEDULE", "LOCK", rules[i].note.c_str()); }
    }
  }
}

// ---------- MQTT ----------
void buildTopics() {
  T_CMD = "locker/" + String(DOOR_ID) + "/cmd";
  T_STATUS = "locker/" + String(DOOR_ID) + "/status";
  T_EVENT = "locker/" + String(DOOR_ID) + "/event";
  T_CARDS_ADD = "locker/" + String(DOOR_ID) + "/cards/add";
  T_CARDS_REMOVE = "locker/" + String(DOOR_ID) + "/cards/remove";
  T_CARDS_LIST_REQ = "locker/" + String(DOOR_ID) + "/cards/list/get";
  T_CARDS_LIST_RESP = "locker/" + String(DOOR_ID) + "/cards/list";
  T_SCHED_SET = "locker/" + String(DOOR_ID) + "/schedule/set";
  T_SCHED_GET = "locker/" + String(DOOR_ID) + "/schedule/get";
  T_SCHED_CUR = "locker/" + String(DOOR_ID) + "/schedule";
}

void onMqtt(char* topic, byte* payload, unsigned int len) {
  String t = String(topic);
  String msg; msg.reserve(len + 1);
  for (unsigned i = 0; i < len; ++i) msg += (char)payload[i];
  msg.trim();
  Serial.printf("MQTT in: %s -> (len=%u)\n", t.c_str(), (unsigned)msg.length());

  if (t == T_CMD) {
    if (msg == "LOCK") { setRelayState(false, false); publishStatus(); logEvent("MQTT", "LOCK"); }
    else if (msg == "UNLOCK") { setRelayState(true, false); publishStatus(); logEvent("MQTT", "UNLOCK"); }
    else if (msg == "ENROLL_ON") { enrollMode = true; enrollTs = millis(); logEvent("MQTT", "ENROLL_ON"); }
    else if (msg == "ENROLL_OFF") { enrollMode = false; logEvent("MQTT", "ENROLL_OFF"); }
    return;
  }

  if (t == T_SCHED_SET) {
    Serial.printf("SCHED SET received len=%u\n", (unsigned)msg.length());
    if (parseScheduleJson(msg)) { saveScheduleFS(msg); mqtt.publish(T_SCHED_CUR.c_str(), msg.c_str(), true); logEvent("MQTT", "SCHEDULE_SAVE"); }
    else { Serial.println("SCHED SET parse failed"); logEvent("MQTT", "SCHEDULE_PARSE_FAIL"); }
    return;
  }

  if (t == T_SCHED_GET) {
    String s = scheduleAsJson(); mqtt.publish(T_SCHED_CUR.c_str(), s.c_str(), true); logEvent("MQTT", "SCHEDULE_GET"); return;
  }

  if (t == T_CARDS_LIST_RESP) {
    Serial.printf("Cards list response len=%u\n", (unsigned)msg.length());
    size_t cap = msg.length() + 2048; if (cap > 32768) cap = 32768;
    DynamicJsonDocument doc(cap);
    DeserializationError err = deserializeJson(doc, msg);
    if (err) { Serial.printf("cards list parse err: %s\n", err.c_str()); return; }
    JsonArray arr = doc.as<JsonArray>(); if (arr.isNull()) { Serial.println("cards list not array"); return; }
    allowedCards.clear();
    for (JsonVariant v : arr) {
      JsonObject o = v.as<JsonObject>();
      if (!o.isNull()) {
        CardEntry e; e.card = (unsigned long)(o["card"] | 0UL);
        e.first = String((const char*)(o["first"] | "")); e.last = String((const char*)(o["last"] | ""));
        if (e.card) allowedCards.push_back(e);
      } else {
        unsigned long long num = v.as<unsigned long long>();
        if (num) { CardEntry e; e.card = (unsigned long)num; e.first = ""; e.last = ""; allowedCards.push_back(e); }
      }
    }
    if (!saveCardsToFS()) Serial.println("Warning: saving cards to FS failed");
    Serial.printf("Cards cache updated: %u entries\n", (unsigned)allowedCards.size());
    return;
  }

  if (t.endsWith("/cards/add/resp") || t.endsWith("/cards/remove/resp")) {
    Serial.printf("Cards resp: %s\n", msg.c_str());
    logEvent("MQTT", "CARDS_RESP");
    return;
  }
}

void ensureMqtt() {
  if (mqtt.connected()) return;
  String cid = "ESP_door_" + String(DOOR_ID) + "_" + String(ESP.getChipId(), HEX);
  Serial.printf("MQTT connecting as %s\n", cid.c_str());
  mqtt.setCallback(onMqtt);
  if (mqtt.connect(cid.c_str())) {
    Serial.println("MQTT connected");
    mqtt.subscribe(T_CMD.c_str());
    mqtt.subscribe(T_SCHED_SET.c_str());
    mqtt.subscribe(T_SCHED_GET.c_str());
    mqtt.subscribe(T_CARDS_LIST_RESP.c_str());
    mqtt.subscribe((String("locker/") + String(DOOR_ID) + "/cards/add/resp").c_str());
    mqtt.subscribe((String("locker/") + String(DOOR_ID) + "/cards/remove/resp").c_str());
    publishStatus();
    publishInfo(); // publică și info (ip + freeHeap)
    mqtt.publish(T_CARDS_LIST_REQ.c_str(), "");
    Serial.println("Requested cards list on connect.");
  } else {
    Serial.printf("MQTT connect failed rc=%d\n", mqtt.state());
    delay(2000);
  }
}

// ---------- WEB UI (index) ----------
const char index_html[] PROGMEM = R"rawliteral(
<!doctype html>
<html lang="ro">
<head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP Vestiar — Programări & Carduri</title>
<style>
:root{--bg:#0b1220;--card:#0f172a;--bd:#334155;--ok:#22c55e;--warn:#f59e0b}
body{background:var(--bg);color:#e6eef6;font-family:Arial;padding:12px}
.container{max-width:1000px;margin:0 auto}
h1{font-size:20px;margin:0 0 8px}
.panel{background:var(--card);border:1px solid var(--bd);padding:12px;border-radius:8px}
.row{display:flex;gap:8px;align-items:center;margin-bottom:8px;flex-wrap:wrap}
.chips{display:flex;gap:6px;flex-wrap:wrap}
.chip{padding:6px 8px;border-radius:999px;border:1px solid var(--bd);cursor:pointer;user-select:none}
.chip.active{background:#123; color:#cfe}
.rule{display:grid;grid-template-columns:1fr 110px 120px 1fr 36px;gap:8px;align-items:center;padding:8px;border-bottom:1px solid rgba(255,255,255,0.02)}
.small{font-size:13px;color:#9fb0c6}
.note{font-size:13px;opacity:.85;margin-top:8px}
.button{background:#0b4fff;color:#fff;padding:8px 10px;border-radius:6px;border:none;cursor:pointer}
.del{background:#172036;border:1px solid var(--bd);border-radius:6px;padding:6px 8px;cursor:pointer}
.footer{display:flex;gap:8px;justify-content:flex-end;margin-top:10px}
.table{width:100%;border-collapse:collapse;margin-top:8px}
.table th{font-weight:700;text-align:left;padding:6px 8px;border-bottom:1px solid rgba(255,255,255,0.04)}
.table td{padding:6px 8px;border-bottom:1px solid rgba(255,255,255,0.02);font-size:13px}
</style>
</head>
<body>
<div class="container">
  <h1>ESP Vestiar — Programări & Carduri</h1>

  <div class="panel">
    <div class="row">
      <div>Ușă: <span id="doorId" class="small">—</span></div>
      <div>IP: <span id="ip" class="small">—</span></div>
      <div>Ora: <span id="now" class="small">—</span></div>
      <div>Stare: <span id="lock" class="small">—</span></div>
      <div style="margin-left:auto"><button id="btnReload" class="button">Reîncarcă</button></div>
    </div>

    <h3>Programări</h3>
    <div id="rulesContainer"></div>

    <div class="footer">
      <button id="btnAdd" class="button">Adaugă regulă</button>
      <button id="btnSave" class="button">Salvează în ESP</button>
      <button id="btnClear" class="button">Șterge toate</button>
    </div>

    <div class="note">Selectează zilele, ora și acțiunea. Noile reguli au implicit Luni–Vineri selectate.</div>
  </div>

  <div style="height:12px"></div>

  <div class="panel">
    <h3>Carduri (cache)</h3>
    <div class="row">
      <button id="btnCardsRefresh" class="button">Actualizează din server</button>
      <button id="btnAddCard" class="button">Adaugă card manual</button>
      <div id="cardsMsg" class="small" style="margin-left:auto"></div>
    </div>

    <!-- Informații DB (PHP) -->
    <div id="dbInfo" class="small" style="margin-top:6px">
      Ultimul card din DB: <span id="dbLast">—</span> •
      Distincte: <span id="dbDistinct">—</span> •
      Evenimente CARD: <span id="dbTotal">—</span>
    </div>

    <table class="table" id="cardsTable">
      <thead><tr><th>Card</th><th>Nume</th><th>Acțiune</th></tr></thead>
      <tbody id="cardsBody"></tbody>
    </table>
  </div>
</div>

<!-- Dialog simplu -->
<div id="cardPrompt" style="display:none;position:fixed;inset:0;background:rgba(0,0,0,.6);align-items:center;justify-content:center">
  <div style="background:#071018;padding:16px;border-radius:8px;max-width:360px;margin:auto">
    <div style="margin-bottom:8px">Adaugă card</div>
    <div style="display:flex;flex-direction:column;gap:6px">
      <input id="cardNum" placeholder="card numeric" style="padding:8px;border-radius:6px;background:#0b1b2a;color:#fff;border:1px solid #233">
      <input id="cardFirst" placeholder="prenume (opțional)" style="padding:8px;border-radius:6px;background:#0b1b2a;color:#fff;border:1px solid #233">
      <input id="cardLast" placeholder="nume (opțional)" style="padding:8px;border-radius:6px;background:#0b1b2a;color:#fff;border:1px solid #233">
      <div style="display:flex;gap:8px;justify-content:flex-end">
        <button id="cardAddOk" class="button">Adaugă</button>
        <button id="cardAddCancel" class="button" style="background:#666">Anulează</button>
      </div>
    </div>
  </div>
</div>

<script>
const api = (path, method='GET', body=null) => {
  const opts = { method, headers: {} };
  if (body) { opts.headers['Content-Type'] = 'application/json'; opts.body = JSON.stringify(body); }
  return fetch(path, opts).then(r=>r.ok?r.json():r.text().then(t=>{throw t}));
};

// <<< SCHIMBĂ cu IP-ul serverului tău PHP >>>
const DBINFO_URL = 'http://192.168.66.240/vestiar_dbinfo.php'; // ex: + '?device_id=5'

let rules = []; // [{days:[0..6], time:'08:00', action:'UNLOCK', note:''}, ...]
const dayNames = ['Lu','Ma','Mi','Jo','Vi','Sa','Du'];

function makeRuleElement(r, idx){
  const el = document.createElement('div'); el.className='rule';
  const daysWrap = document.createElement('div'); daysWrap.className='chips';
  dayNames.forEach((n,d)=>{
    const c = document.createElement('div'); c.className='chip'; c.textContent = n;
    if ((r.days||[]).includes(d)) c.classList.add('active');
    c.onclick = ()=>{
      if (!r.days) r.days = [];
      if (r.days.includes(d)) r.days = r.days.filter(x=>x!==d);
      else r.days.push(d);
      renderRules();
    };
    daysWrap.appendChild(c);
  });
  const t = document.createElement('input'); t.type='time'; t.value = r.time || '08:00'; t.oninput = ()=> r.time = t.value;
  const sel = document.createElement('select');
  ['UNLOCK','LOCK'].forEach(a=>{ const o = document.createElement('option'); o.value=a; o.textContent=a; sel.appendChild(o); });
  sel.value = r.action || 'UNLOCK'; sel.onchange = ()=> r.action = sel.value;
  const note = document.createElement('input'); note.type='text'; note.value = r.note || ''; note.placeholder='notă (opțional)'; note.oninput = ()=> r.note = note.value;
  const del = document.createElement('button'); del.className='del'; del.textContent='✕'; del.onclick = ()=> { rules.splice(idx,1); renderRules(); };
  el.appendChild(daysWrap); el.appendChild(t); el.appendChild(sel); el.appendChild(note); el.appendChild(del);
  return el;
}
function renderRules(){
  const cont = document.getElementById('rulesContainer'); cont.innerHTML='';
  rules.forEach((r,i)=> cont.appendChild(makeRuleElement(r,i)));
}
function loadSchedule(){
  api('/api/schedule').then(j=>{
    rules = j.rules || [];
    rules = rules.map(r=>{
      if (!r.days || !Array.isArray(r.days) || r.days.length===0) r.days = [0,1,2,3,4];
      return { days: r.days.slice(), time: r.time||'08:00', action: r.action||'UNLOCK', note: r.note||'' };
    });
    renderRules();
  }).catch(e=>{ console.warn('load schedule err', e); rules = [{ days:[0,1,2,3,4], time:'08:00', action:'UNLOCK', note:'' }]; renderRules(); });
}
function saveSchedule(){
  const payload = { rules: rules.map(r=>({ days: r.days.slice(), time: r.time, action: r.action, note: r.note })) };
  api('/api/schedule','POST',payload).then(()=>{ alert('Salvat'); }).catch(e=>{ alert('Eroare salvare'); console.error(e); });
}
document.getElementById('btnAdd').onclick = ()=>{ rules.push({ days:[0,1,2,3,4], time:'08:00', action:'UNLOCK', note:'' }); renderRules(); };
document.getElementById('btnSave').onclick = saveSchedule;
document.getElementById('btnClear').onclick = ()=>{ rules = []; renderRules(); };
document.getElementById('btnReload').onclick = ()=>{ fetchInfo(); loadSchedule(); };

function fetchInfo(){
  api('/api/now')
    .then(j=>{
      document.getElementById('now').textContent = j.now;
    })
    .catch(()=>{});

  api('/api/status')
    .then(j=>{
      document.getElementById('lock').textContent   = j.lock;
      document.getElementById('ip').textContent     = j.ip;
      document.getElementById('doorId').textContent = j.door_id; // 🔐
    })
    .catch(()=>{});
}



/* Carduri */
function renderCards(list){
  const body = document.getElementById('cardsBody'); body.innerHTML='';
  list.forEach(c=>{
    const tr = document.createElement('tr');
    const td1 = document.createElement('td'); td1.textContent = c.card;
    const td2 = document.createElement('td'); td2.textContent = (c.first||'') + (c.last?(' '+c.last):'');
    const td3 = document.createElement('td');
    const del = document.createElement('button'); del.textContent='Șterge'; del.className='button'; del.onclick = ()=> removeCard(c.card);
    td3.appendChild(del);
    tr.appendChild(td1); tr.appendChild(td2); tr.appendChild(td3); body.appendChild(tr);
  });
}
function loadCards(){ api('/api/cards').then(j=>{ renderCards(j.cards||[]); }).catch(()=>{}); }
document.getElementById('btnCardsRefresh').onclick = ()=> { api('/api/cards/refresh','POST',{}).then(()=>{ loadCards(); loadDbInfo(); }); };
function removeCard(card){
  if (!confirm('Șterge card ' + card + '?')) return;
  api('/api/cards/remove','POST',{ card: Number(card) }).then(()=>{ loadCards(); loadDbInfo(); }).catch(console.error);
}
document.getElementById('btnAddCard').onclick = ()=> { document.getElementById('cardPrompt').style.display='flex'; };
document.getElementById('cardAddCancel').onclick = ()=> { document.getElementById('cardPrompt').style.display='none'; };

// Citește informații din DB (PHP)
function loadDbInfo(){
  fetch(DBINFO_URL).then(r=>r.json()).then(j=>{
    if(!j || !j.ok) return;
    document.getElementById('dbLast').textContent     = j.last_card24 ?? '—';
    document.getElementById('dbDistinct').textContent = j.distinct_cards ?? '—';
    document.getElementById('dbTotal').textContent    = j.total_events ?? '—';
    const inp = document.getElementById('cardNum');
    if (inp && j.last_card24) inp.placeholder = `ultimul: ${j.last_card24}`;
  }).catch(()=>{});
}

document.getElementById('cardAddOk').onclick = ()=>{
  const card = document.getElementById('cardNum').value.trim();
  const first = document.getElementById('cardFirst').value.trim();
  const last = document.getElementById('cardLast').value.trim();
  if (!card) { alert('Introdu card'); return; }
  api('/api/cards/add','POST',{ card: Number(card), first: first, last: last })
    .then(()=>{ document.getElementById('cardPrompt').style.display='none'; loadCards(); loadDbInfo(); })
    .catch(()=>{ alert('Eroare'); });
};

/* init */
fetchInfo();
loadSchedule();
loadCards();
loadDbInfo();
setInterval(()=>{ fetchInfo(); loadDbInfo(); }, 5000);
</script>
</body>
</html>
)rawliteral";

// ---------- WEB API helpers ----------
void sendJsonWithCORS(int code, const String& body) {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
  server.send(code, "application/json", body);
}

void handleRoot() { String html = FPSTR(index_html); server.sendHeader("Access-Control-Allow-Origin", "*"); server.send(200, "text/html", html); }
void handleNow() { DynamicJsonDocument d(128); d["now"] = nowStamp(); String out; serializeJson(d, out); sendJsonWithCORS(200, out); }
void handleMem() { DynamicJsonDocument d(128); d["freeHeap"] = (unsigned)ESP.getFreeHeap(); String out; serializeJson(d, out); sendJsonWithCORS(200, out); }
void handleStatus() {
  DynamicJsonDocument d(256);
  d["lock"] = lockState;
  d["ts"]   = nowStamp();
  d["ip"]   = ipStr();
  d["mac"]  = WiFi.macAddress();
  d["rssi"] = (int)WiFi.RSSI();
  d["door"] = (digitalRead(PIN_DOOR) == LOW) ? "CLOSED" : "OPEN";
  d["pwr"]  = (digitalRead(PIN_PWR)  == HIGH) ? "ON"     : "OFF";

  // 🔐 număr ușă mascat (ex: **2, **12, **34)
  String masked = "" + String(DOOR_ID);
  d["door_id"] = masked;

  String out;
  serializeJson(d, out);
  sendJsonWithCORS(200, out);
}



// Trimite evenimente CARD la serverul PHP

void sendEventToServer(unsigned long card24, unsigned int facility, unsigned int cardNumber, bool ok) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected, skip HTTP send");
    return;
  }

  WiFiClient client;                  // ✅ creează clientul WiFi
  HTTPClient http;                    // ✅ creează obiectul HTTP

  // ✅ noua sintaxă: http.begin(client, "url")
  http.begin(client, "http://192.168.66.240/vestiar_event.php");
  http.addHeader("Content-Type", "application/json");

  DynamicJsonDocument d(256);
  d["type"] = "CARD";
  d["source"] = String("door_") + String(DOOR_ID);
  d["card24"] = card24;
  d["facility"] = facility;
  d["cardNumber"] = cardNumber;
  d["ok"] = ok;
  d["first"] = "";
  d["last"] = "";

  String json;
  serializeJson(d, json);

  int code = http.POST(json);
  Serial.printf("HTTP POST event (%lu) -> code=%d\n", card24, code);
  http.end();
}
void handleGetCards() {
  DynamicJsonDocument doc(32768);
  JsonArray arr = doc.createNestedArray("cards");
  for (auto &c : allowedCards) { JsonObject o = arr.createNestedObject(); o["card"] = c.card; if (c.first.length()) o["first"] = c.first; if (c.last.length()) o["last"] = c.last; }
  String out; serializeJson(doc, out); sendJsonWithCORS(200, out);
}
void handlePostCardsAdd() {
  if (!server.hasArg("plain")) { sendJsonWithCORS(400, "{\"ok\":false}"); return; }
  String body = server.arg("plain");
  size_t cap = body.length() + 256; if (cap > 8192) cap = 8192;
  DynamicJsonDocument d(cap);
  if (deserializeJson(d, body)) { sendJsonWithCORS(400, "{\"ok\":false}"); return; }
  unsigned long card = (unsigned long)(d["card"] | 0UL);
  String first = String((const char*)(d["first"] | ""));
  String last  = String((const char*)(d["last"]  | ""));
  if (!card) { sendJsonWithCORS(400, "{\"ok\":false}"); return; }
  DynamicJsonDocument p(512); p["card"] = card; if (first.length()) p["first"] = first; if (last.length()) p["last"] = last;
  String payload; serializeJson(p, payload); if (T_CARDS_ADD.length()) mqtt.publish(T_CARDS_ADD.c_str(), payload.c_str(), false);
  bool exists = false; for (auto &c : allowedCards) if (c.card == card) { exists = true; break; }
  if (!exists) { CardEntry e; e.card = card; e.first = first; e.last = last; allowedCards.push_back(e); saveCardsToFS(); logEvent("CARD_ADD", "WEB"); }
  sendJsonWithCORS(200, "{\"ok\":true}");
}
void handlePostCardsRemove() {
  if (!server.hasArg("plain")) { sendJsonWithCORS(400, "{\"ok\":false}"); return; }
  String body = server.arg("plain");
  DynamicJsonDocument d(512); if (deserializeJson(d, body)) { sendJsonWithCORS(400, "{\"ok\":false}"); return; }
  unsigned long card = (unsigned long)(d["card"] | 0UL);
  if (!card) { sendJsonWithCORS(400, "{\"ok\":false}"); return; }
  DynamicJsonDocument p(256); p["card"] = card; String py; serializeJson(p, py); if (T_CARDS_REMOVE.length()) mqtt.publish(T_CARDS_REMOVE.c_str(), py.c_str(), false);
  for (size_t i = 0; i < allowedCards.size(); ++i) if (allowedCards[i].card == card) { allowedCards.erase(allowedCards.begin() + i); break; }
  saveCardsToFS(); logEvent("CARD_REMOVE", "WEB"); sendJsonWithCORS(200, "{\"ok\":true}");
}
void handlePostCardsRefresh() { if (T_CARDS_LIST_REQ.length()) mqtt.publish(T_CARDS_LIST_REQ.c_str(), ""); sendJsonWithCORS(200, "{\"ok\":true}"); }

void handleGetSchedule() {
  if (!LittleFS.exists(FILE_SCHEDULE)) {
    DynamicJsonDocument doc(1024);
    JsonArray arr = doc.createNestedArray("rules");
    JsonObject o = arr.createNestedObject();
    JsonArray ds = o.createNestedArray("days");
    ds.add(0); ds.add(1); ds.add(2); ds.add(3); ds.add(4);
    o["time"] = "08:00"; o["action"] = "UNLOCK"; o["note"] = "Implicit L–V";
    String out; serializeJson(doc, out); sendJsonWithCORS(200, out); return;
  }
  File f = LittleFS.open(FILE_SCHEDULE, "r"); if (!f) { sendJsonWithCORS(500, "{\"ok\":false}"); return; }
  String s = f.readString(); f.close(); sendJsonWithCORS(200, s);
}
void handlePostSchedule() {
  if (!server.hasArg("plain")) { sendJsonWithCORS(400, "{\"ok\":false}"); return; }
  String body = server.arg("plain");
  Serial.printf("HTTP POST /api/schedule len=%u\n", (unsigned)body.length());
  bool ok = parseScheduleJson(body);
  if (!ok) { sendJsonWithCORS(400, "{\"ok\":false,\"err\":\"parse\"}"); return; }
  bool saved = saveScheduleFS(body);
  if (!saved) { sendJsonWithCORS(500, "{\"ok\":false,\"err\":\"save\"}"); return; }
  if (T_SCHED_CUR.length()) mqtt.publish(T_SCHED_CUR.c_str(), body.c_str(), true);
  sendJsonWithCORS(200, "{\"ok\":true}");
}

void handleCmd() {
  if (!server.hasArg("plain")) { 
    sendJsonWithCORS(400, "{\"ok\":false}"); 
    return; 
  }

  String body = server.arg("plain");
  DynamicJsonDocument d(256); 
  if (deserializeJson(d, body)) { 
    sendJsonWithCORS(400, "{\"ok\":false}"); 
    return; 
  }

  String cmd = String((const char*)(d["cmd"] | ""));

  if (cmd == "UNLOCK") {
    // 🔹 Deschis din browser → rămâne deschis până la LOCK
    setRelayState(true, false, false);  
    publishStatus();
    logEvent("HTTP", "UNLOCK");
    sendJsonWithCORS(200, "{\"ok\":true}");
  } 
  else if (cmd == "LOCK") {
    setRelayState(false, false, false);
    publishStatus();
    logEvent("HTTP", "LOCK");
    sendJsonWithCORS(200, "{\"ok\":true}");
  } 
  else {
    sendJsonWithCORS(400, "{\"ok\":false}");
  }
}

void handleOptions() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
  server.send(204, "text/plain", "");
}

// ---------- SETUP / LOOP ----------
void ICACHE_RAM_ATTR handleEnrollIRQ() { enrollRequest = true; }

void setup() {
  Serial.begin(115200); delay(10);
  Serial.println("\n=== ESP Vestiar Complete ===");

  pinMode(PIN_RELAY, OUTPUT); digitalWrite(PIN_RELAY, RELAY_ACTIVE_LOW ? HIGH : LOW);

  pinMode(PIN_KPD_D0, INPUT_PULLUP); pinMode(PIN_KPD_D1, INPUT_PULLUP);
  pinMode(PIN_RFID_D0, INPUT_PULLUP);
  // IMPORTANT: GPIO15 (D8) trebuie LOW la boot; folosește rezistor 10k la GND.
  pinMode(PIN_RFID_D1, INPUT);

  attachInterrupt(digitalPinToInterrupt(PIN_KPD_D0), handleKPD_D0, FALLING);
  attachInterrupt(digitalPinToInterrupt(PIN_KPD_D1), handleKPD_D1, FALLING);
  attachInterrupt(digitalPinToInterrupt(PIN_RFID_D0), handleRFID_D0, FALLING);
  attachInterrupt(digitalPinToInterrupt(PIN_RFID_D1), handleRFID_D1, FALLING);

  pinMode(PIN_ENROLL, INPUT_PULLUP); attachInterrupt(digitalPinToInterrupt(PIN_ENROLL), handleEnrollIRQ, FALLING);

  pinMode(PIN_DOOR, INPUT_PULLUP); pinMode(PIN_PWR, INPUT_PULLUP);

  if (!LittleFS.begin()) Serial.println("LittleFS begin failed");
  else {
    Serial.println("LittleFS OK");
    loadCardsFromFS();
    if (LittleFS.exists(FILE_SCHEDULE)) {
      File f = LittleFS.open(FILE_SCHEDULE, "r");
      if (f) { String s = f.readString(); f.close(); if (!parseScheduleJson(s)) Serial.println("Saved schedule parse failed"); else Serial.println("Loaded schedule from FS"); }
    } else {
      DynamicJsonDocument doc(512); JsonArray arr = doc.createNestedArray("rules");
      JsonObject o = arr.createNestedObject(); JsonArray ds = o.createNestedArray("days"); ds.add(0); ds.add(1); ds.add(2); ds.add(3); ds.add(4);
      o["time"] = "08:00"; o["action"] = "UNLOCK"; o["note"] = "Implicit L–V";
      String out; serializeJson(doc, out); saveScheduleFS(out); parseScheduleJson(out);
      Serial.println("Created default schedule (Mon-Fri 08:00 UNLOCK)");
    }
  }

  buildTopics();
  mqtt.setServer(MQTT_HOST, MQTT_PORT);

  WiFi.mode(WIFI_STA); WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Connecting WiFi");
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(500); Serial.print(".");
    if (millis() - t0 > 30000) { Serial.println("\nWiFi timeout, restart"); ESP.restart(); }
  }
  Serial.printf("\nWiFi OK, IP=%s\n", WiFi.localIP().toString().c_str());

  configTime(TZ_OFFSET, DST_OFFSET, NTP1, NTP2);

  // Web routes
  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/now", HTTP_GET, handleNow);
  server.on("/api/mem", HTTP_GET, handleMem);
  server.on("/api/status", HTTP_GET, handleStatus);

  server.on("/api/cards", HTTP_GET, handleGetCards);
  server.on("/api/cards/add", HTTP_POST, handlePostCardsAdd);
  server.on("/api/cards/remove", HTTP_POST, handlePostCardsRemove);
  server.on("/api/cards/refresh", HTTP_POST, handlePostCardsRefresh);

  server.on("/api/schedule", HTTP_GET, handleGetSchedule);
  server.on("/api/schedule", HTTP_POST, handlePostSchedule);

  server.on("/api/cmd", HTTP_POST, handleCmd);

  // OPTIONS CORS
  server.on("/api/cards/add", HTTP_OPTIONS, handleOptions);
  server.on("/api/cards/remove", HTTP_OPTIONS, handleOptions);
  server.on("/api/cards/refresh", HTTP_OPTIONS, handleOptions);
  server.on("/api/schedule", HTTP_OPTIONS, handleOptions);
  server.on("/api/cmd", HTTP_OPTIONS, handleOptions);

  server.onNotFound([]() { server.send(404, "text/plain", "Not found"); });
  server.begin();
  Serial.println("HTTP server started");
  printMemoryInfo();

  // Trimite status & info imediat (retained)
  publishStatus();
  publishInfo();
}

void loop() {
  server.handleClient();
if (relayAutoLockActive && millis() - lastRelayOn >= 3000UL) {
  digitalWrite(PIN_RELAY, RELAY_ACTIVE_LOW ? HIGH : LOW);
  relayAutoLockActive = false;
  lockState = "LOCKED";
  Serial.println("Relay AUTO-LOCK după 3s (card/parolă)");
  publishStatus();
}
  if (enrollRequest) { enrollRequest = false; enrollMode = true; enrollTs = millis(); logEvent("ENROLL", "ON"); Serial.println("Enroll ON"); }
  if (enrollMode && (millis() - enrollTs > ENROLL_TIMEOUT)) { enrollMode = false; logEvent("ENROLL", "TIMEOUT"); Serial.println("Enroll TIMEOUT"); }

  if (!mqtt.connected()) ensureMqtt();
  mqtt.loop();

  processWiegand();
  evaluateScheduleEachMinute();

  // publish status când se schimbă senzori
  static String lastPack = "";
  String door = (digitalRead(PIN_DOOR) == LOW) ? "CLOSED" : "OPEN";
  String pwr  = (digitalRead(PIN_PWR)  == HIGH) ? "ON"     : "OFF";
  String pack = lockState + "/" + door + "/" + pwr;
  if (pack != lastPack) { lastPack = pack; publishStatus(); }

  // re-trimite info la schimbări IP/RSSI/freeHeap (opțional)
  static String lastIpSent = "";
  static long   lastRssiSent = 0;
  static unsigned lastHeapSent = 0;
  String ip = ipStr();
  long rssi = WiFi.RSSI();
  unsigned heap = (unsigned)ESP.getFreeHeap();
  if (ip != lastIpSent || abs(rssi - lastRssiSent) >= 8 || abs((int)heap-(int)lastHeapSent) > 2048) {
    lastIpSent = ip; lastRssiSent = rssi; lastHeapSent = heap;
    publishStatus();
    publishInfo();
  }

  if (millis() - lastSecondPrint > 1000) { lastSecondPrint = millis(); time_t now = time(nullptr); if (now > 100000) Serial.println(nowStamp()); }

  static unsigned long lastMem = 0;
  if (millis() - lastMem > 60000) { printMemoryInfo(); lastMem = millis(); }

  delay(5);
}
