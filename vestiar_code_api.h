#pragma once
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <ESP8266WebServer.h>

extern ESP8266WebServer server;
extern String masterCode;
extern String lockState;

// === CONFIG fișier pentru codul keypad ===
static const char* FILE_CODE = "/code.json";

// ---------- Helper pentru CORS ----------
inline void sendJsonWithCORS(int code, const String& body) {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
  server.send(code, "application/json", body);
}

// ---------- Funcții pentru cod ----------
inline bool loadCode() {
  if (!LittleFS.exists(FILE_CODE)) return false;
  File f = LittleFS.open(FILE_CODE, "r");
  if (!f) return false;
  DynamicJsonDocument d(128);
  if (deserializeJson(d, f)) { f.close(); return false; }
  f.close();
  const char* c = d["code"] | nullptr;
  if (!c) return false;
  masterCode = String(c);
  Serial.printf("Loaded keypad masterCode (%u chars)\n", (unsigned)masterCode.length());
  return true;
}

inline bool saveCode(const String& code) {
  DynamicJsonDocument d(128);
  d["code"] = code;
  File f = LittleFS.open(FILE_CODE, "w");
  if (!f) return false;
  serializeJson(d, f);
  f.close();
  masterCode = code;
  return true;
}

// ---------- Endpoints API ----------
inline void handleCodeGet() {
  DynamicJsonDocument d(128);
  d["set"] = masterCode.length() > 0;
  d["len"] = (int)masterCode.length();
  String out; serializeJson(d, out);
  sendJsonWithCORS(200, out);
}

inline void handleCodePost() {
  if (!server.hasArg("plain")) { sendJsonWithCORS(400, "{\"ok\":false,\"err\":\"no_body\"}"); return; }
  DynamicJsonDocument d(256);
  if (deserializeJson(d, server.arg("plain"))) { sendJsonWithCORS(400, "{\"ok\":false,\"err\":\"json\"}"); return; }

  String current = String((const char*)(d["current"] | ""));
  String news    = String((const char*)(d["new"] | ""));
  String confirm = String((const char*)(d["confirm"] | ""));

  auto isDigits = [](const String& s)->bool {
    for (size_t i=0;i<s.length();++i){ if (s[i]<'0'||s[i]>'9') return false; }
    return true;
  };

  if (news.length() < 4 || news.length() > 8 || !isDigits(news)) {
    sendJsonWithCORS(400, "{\"ok\":false,\"err\":\"fmt\",\"hint\":\"4-8 cifre\"}");
    return;
  }
  if (news != confirm) { sendJsonWithCORS(400, "{\"ok\":false,\"err\":\"confirm\"}"); return; }

  if (masterCode.length() > 0 && current != masterCode) {
    sendJsonWithCORS(403, "{\"ok\":false,\"err\":\"current\"}");
    return;
  }

  if (!saveCode(news)) {
    sendJsonWithCORS(500, "{\"ok\":false,\"err\":\"save\"}");
    return;
  }

  Serial.println("Master code changed successfully!");
  sendJsonWithCORS(200, "{\"ok\":true}");
}

// ---------- Înregistrare rute ----------
inline void registerCodeApi() {
  server.on("/api/code", HTTP_GET,  handleCodeGet);
  server.on("/api/code", HTTP_POST, handleCodePost);
  server.on("/api/code", HTTP_OPTIONS, [](){
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.sendHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
    server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
    server.send(204, "text/plain", "");
  });
}
