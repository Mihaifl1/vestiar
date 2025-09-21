/****************************************************
 * ESP8266 – Vestiar cu programari simple (Zile + Ora + Actiune)
 * - WiFi + MQTT
 * - NTP (ora locala) + print in Serial la fiecare secunda
 * - Comenzi:   locker/<ID>/cmd          ("LOCK" | "UNLOCK")
 * - Status:    locker/<ID>/status       (JSON retained)
 * - Programare:
 *     set:     locker/<ID>/schedule/set (JSON {rules:[...]})
 *     get:     locker/<ID>/schedule/get ("REQ")
 *     curent:  locker/<ID>/schedule     (JSON {rules:[...]})
 ****************************************************/

#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <time.h>

// ----------- WiFi -----------
const char* WIFI_SSID = "1";
const char* WIFI_PASS = "";

// ----------- MQTT -----------
const char* MQTT_HOST = "broker.emqx.io";
const uint16_t MQTT_PORT = 1883;

// ----------- Ușa curentă -----------
const int DOOR_ID = 1;            // setează 1..6 pentru fiecare modul

// ----------- Releu + senzori -----------
const int PIN_RELAY = D1;         // -> modul releu
const int PIN_DOOR  = D2;         // reed switch (LOW=CLOSED) cu INPUT_PULLUP
const int PIN_PWR   = D3;         // detect alimentare (HIGH=ON) cu INPUT_PULLUP
const bool RELAY_ACTIVE_LOW = false;   // true dacă releul tău e activ pe LOW
const unsigned RELAY_PULSE_MS = 400;   // durată puls pentru LOCK/UNLOCK (0 = menținere)

// ----------- Fus orar (simplu) -----------
/* Varianta simpla (fixa): UTC+2, DST +1
 * Pentru TZ POSIX exact (EET-2EEST,M3.5.0/3,M10.5.0/4) poti folosi setenv("TZ", "...", 1); tzset();
 */
const long   TZ_OFFSET  = 2 * 3600;   // +02:00
const long   DST_OFFSET = 1 * 3600;   // +01:00 (vara)
const char*  NTP1 = "pool.ntp.org";
const char*  NTP2 = "time.nist.gov";

// ----------- MQTT topics -----------
String T_CMD, T_STATUS, T_SCHED_SET, T_SCHED_GET, T_SCHED_CUR;

// ----------- Obiecte -----------
WiFiClient wifi;
PubSubClient mqtt(wifi);

// ----------- Stare curentă -----------
String lockState  = "LOCKED";    // LOCKED | UNLOCKED
String doorState  = "CLOSED";    // CLOSED | OPEN
String powerState = "ON";        // ON | OFF

// ----------- Programare simplă -----------
/* Regula: zile (0-6 Luni..Duminica), time "HH:MM", action "LOCK|UNLOCK", note optional */
struct Rule {
  bool days[7];
  uint16_t minuteOfDay;  // HH*60 + MM
  String action;         // "LOCK" | "UNLOCK"
  String note;           // optional
};
#define MAX_RULES 32
Rule rules[MAX_RULES];
int  rulesCount = 0;

String timeToStr(time_t ts) {
  struct tm t; localtime_r(&ts, &t);
  char buf[20]; strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &t);
  return String(buf);
}
String curTimeStr() {
  time_t now = time(nullptr); return timeToStr(now);
}

// ----------------- RELAY HELPERS -----------------
void setRelay(bool on) {
  int level = RELAY_ACTIVE_LOW ? (on ? LOW : HIGH) : (on ? HIGH : LOW);
  digitalWrite(PIN_RELAY, level);
}
void pulseRelay() {
  if (RELAY_PULSE_MS > 0) {
    setRelay(true);
    delay(RELAY_PULSE_MS);
    setRelay(false);
  }
}

// ----------------- STATUS -----------------
void publishStatus() {
  StaticJsonDocument<128> d;
  d["lock"]  = lockState;
  d["door"]  = doorState;
  d["power"] = powerState;
  String out; serializeJson(d, out);
  mqtt.publish(T_STATUS.c_str(), out.c_str(), true); // retained
}

void doAction(const String& act) {
  if (act == "UNLOCK") {
    Serial.printf("[%s] ACT: UNLOCK → relay pulse\n", curTimeStr().c_str());
    pulseRelay();
    lockState = "UNLOCKED";
    publishStatus();
  } else if (act == "LOCK") {
    Serial.printf("[%s] ACT: LOCK → relay pulse\n", curTimeStr().c_str());
    pulseRelay();
    lockState = "LOCKED";
    publishStatus();
  } else {
    Serial.printf("[%s] ACT necunoscut: %s\n", curTimeStr().c_str(), act.c_str());
  }
}

// ----------------- FS: schedule.json -----------------
String loadScheduleFS() {
  if (!LittleFS.exists("/schedule.json")) return "";
  File f = LittleFS.open("/schedule.json", "r");
  if (!f) return "";
  String s = f.readString(); f.close();
  return s;
}
void saveScheduleFS(const String& json) {
  File f = LittleFS.open("/schedule.json", "w");
  if (!f) return;
  f.print(json);
  f.close();
}

// Parse JSON → umple rules[]
bool parseSchedule(const String& json) {
  StaticJsonDocument<4096> doc;
  auto err = deserializeJson(doc, json);
  if (err) return false;

  rulesCount = 0;
  for (int i = 0; i < MAX_RULES; i++) for (int d = 0; d < 7; d++) rules[i].days[d] = false;

  JsonArray arr = doc["rules"].as<JsonArray>();
  if (arr.isNull()) return false;

  for (JsonObject r : arr) {
    if (rulesCount >= MAX_RULES) break;
    Rule& R = rules[rulesCount];
    // days
    for (int d = 0; d < 7; d++) R.days[d] = false;
    if (r.containsKey("days")) {
      for (int v : r["days"].as<JsonArray>()) {
        if (v >= 0 && v <= 6) R.days[v] = true;
      }
    }
    // time -> minuteOfDay
    const char* tt = r["time"] | "08:00";
    int H = 8, M = 0; sscanf(tt, "%d:%d", &H, &M);
    R.minuteOfDay = (uint16_t)(H * 60 + M);
    // action
    R.action = String((const char*)(r["action"] | "UNLOCK"));
    R.action.toUpperCase();
    // note
    R.note = String((const char*)(r["note"] | ""));
    rulesCount++;
  }
  Serial.printf("[%s] Program incarcat: %d reguli\n", curTimeStr().c_str(), rulesCount);
  return true;
}

// Publică programul curent (din memorie) la locker/<ID>/schedule
void publishScheduleCur() {
  StaticJsonDocument<4096> doc;
  JsonArray arr = doc.createNestedArray("rules");
  for (int i = 0; i < rulesCount; i++) {
    JsonObject o = arr.createNestedObject();
    JsonArray ds = o.createNestedArray("days");
    for (int d = 0; d < 7; d++) if (rules[i].days[d]) ds.add(d);
    char buf[6]; sprintf(buf, "%02d:%02d", rules[i].minuteOfDay / 60, rules[i].minuteOfDay % 60);
    o["time"] = buf;
    o["action"] = rules[i].action;
    if (rules[i].note.length()) o["note"] = rules[i].note;
  }
  String out; serializeJson(doc, out);
  mqtt.publish(T_SCHED_CUR.c_str(), out.c_str(), false);
}

// ----------------- WiFi & MQTT -----------------
void ensureMqtt() {
  while (!mqtt.connected()) {
    String cid = "ESP_door_" + String(DOOR_ID) + "_" + String(ESP.getChipId(), HEX);
    Serial.printf("[%s] MQTT conectare...\n", curTimeStr().c_str());
    if (mqtt.connect(cid.c_str())) {
      Serial.println("  OK");
      mqtt.subscribe(T_CMD.c_str());
      mqtt.subscribe(T_SCHED_SET.c_str());
      mqtt.subscribe(T_SCHED_GET.c_str());
      publishStatus();
      // la conectare, publica programul din FS daca exista
      String fs = loadScheduleFS();
      if (fs.length()) mqtt.publish(T_SCHED_CUR.c_str(), fs.c_str(), false);
    } else {
      Serial.printf("  Eroare, rc=%d. Retry in 2s\n", mqtt.state());
      delay(2000);
    }
  }
}

void onMqtt(char* topic, byte* payload, unsigned int len) {
  String t = String(topic);
  String msg; msg.reserve(len);
  for (unsigned i = 0; i < len; i++) msg += (char)payload[i];
  msg.trim();

  if (t == T_CMD) {
    Serial.printf("[%s] CMD primit: %s\n", curTimeStr().c_str(), msg.c_str());
    if (msg == "LOCK" || msg == "UNLOCK") doAction(msg);
  }
  else if (t == T_SCHED_SET) {
    Serial.printf("[%s] SCHEDULE/SET primit (%uB)\n", curTimeStr().c_str(), len);
    if (parseSchedule(msg)) {
      saveScheduleFS(msg);
      publishScheduleCur();   // confirma UI
    } else {
      Serial.println("  JSON invalid.");
    }
  }
  else if (t == T_SCHED_GET) {
    Serial.printf("[%s] SCHEDULE/GET\n", curTimeStr().c_str());
    String fs = loadScheduleFS();
    if (fs.length()) mqtt.publish(T_SCHED_CUR.c_str(), fs.c_str(), false);
    else publishScheduleCur(); // din memorie
  }
}

// ----------------- TIME helpers -----------------
int dowMonday0(const tm& t) {  // 0=Luni … 6=Duminica
  int w = t.tm_wday; return (w == 0) ? 6 : (w - 1);
}

unsigned long lastSecondPrint = 0;
unsigned long lastMinuteTick  = 0;

// Execută regulile exact la minutul potrivit (HH:MM)
void evaluateScheduleEachMinute() {
  time_t now = time(nullptr); if (now < 100000) return; // NTP inca
  tm t; localtime_r(&now, &t);
  static int lastMin = -1;
  if (t.tm_min == lastMin) return;      // doar cand se schimba minutul
  lastMin = t.tm_min;

  int day = dowMonday0(t);
  uint16_t curMin = (uint16_t)(t.tm_hour * 60 + t.tm_min);

  for (int i = 0; i < rulesCount; i++) {
    if (!rules[i].days[day]) continue;
    if (rules[i].minuteOfDay == curMin) {
      Serial.printf("[%s] SCHED match (%02d:%02d) → %s  %s\n",
        curTimeStr().c_str(),
        curMin/60, curMin%60,
        rules[i].action.c_str(),
        rules[i].note.c_str());
      doAction(rules[i].action);
    }
  }
}

// ----------------- SETUP/LOOP -----------------
void setup() {
  Serial.begin(115200);
  delay(50);

  pinMode(PIN_RELAY, OUTPUT); setRelay(false);
  pinMode(PIN_DOOR,  INPUT_PULLUP);
  pinMode(PIN_PWR,   INPUT_PULLUP);

  // FS
  LittleFS.begin();

  // Topics
  T_CMD        = "locker/" + String(DOOR_ID) + "/cmd";
  T_STATUS     = "locker/" + String(DOOR_ID) + "/status";
  T_SCHED_SET  = "locker/" + String(DOOR_ID) + "/schedule/set";
  T_SCHED_GET  = "locker/" + String(DOOR_ID) + "/schedule/get";
  T_SCHED_CUR  = "locker/" + String(DOOR_ID) + "/schedule";

  // WiFi
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("[WiFi] Conectare");
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.printf(" OK, IP=%s\n", WiFi.localIP().toString().c_str());

  // MQTT
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMqtt);

  // NTP timp local – varianta simpla: offset TZ + DST
  configTime(TZ_OFFSET, DST_OFFSET, NTP1, NTP2);

  // Încarcă program din FS dacă există
  String fs = loadScheduleFS();
  if (fs.length()) parseSchedule(fs);
}

void loop() {
  if (!mqtt.connected()) ensureMqtt();
  mqtt.loop();

  // Ora in serial la fiecare secunda
  if (millis() - lastSecondPrint > 1000) {
    lastSecondPrint = millis();
    time_t now = time(nullptr);
    if (now > 100000) {
      Serial.println(curTimeStr()); // ex: 2025-09-21 14:05:32
    }
  }

  // Citire senzori si trimitere status la schimbare (door/power)
  static String lastPack;
  String door = (digitalRead(PIN_DOOR) == LOW) ? "CLOSED" : "OPEN";
  String pwr  = (digitalRead(PIN_PWR)  == HIGH) ? "ON" : "OFF";
  String pack = lockState + "/" + door + "/" + pwr;
  if (pack != lastPack) {
    lastPack = pack;
    doorState = door;
    powerState = pwr;
    publishStatus();
  }

  // Evaluare program la schimbare de minut
  evaluateScheduleEachMinute();
}
