#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <DNSServer.h>
#include <WiFiManager.h>  // https://github.com/tzapu/WiFiManager
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include <time.h>
//http://arduino.esp8266.com/stable/package_esp8266com_index.json
// --- Static IP Settings ---
IPAddress local_IP(192, 168, 1, 120);
IPAddress gateway(192, 168, 1, 1);
IPAddress subnet(255, 255, 255, 0);
IPAddress primaryDNS(8, 8, 8, 8);

// --- Time Zone / NTP Settings ---
#define MY_TZ "PHT-8"
const char* ntpServer = "pool.ntp.org";

// --- Pin Setup ---
const int RELAY_PIN = D5;  //1st Hydroponics (seedlings .120)
//const int RELAY_PIN = D2;  //1st Aeroponics (Tower .110)
#define PUMP_ON LOW
#define PUMP_OFF HIGH

ESP8266WebServer server(80);

// --- Shift Structures (Simplified) ---
struct ShiftConfig {
  int startHour;
  unsigned long durationON;    // minutes
  unsigned long durationWAIT;  // minutes (Pump OFF)
  String name;
};

ShiftConfig shift1 = { 6, 1, 12, "Shift 1 (Morning)" };
ShiftConfig shift2 = { 12, 1, 7, "Shift 2 (Day)" };
ShiftConfig shift3 = { 18, 1, 30, "Shift 3 (Night)" };

// Active durations (in seconds)
unsigned long durationON = 60;
unsigned long durationWAIT = 900;
String activeShiftName = "Shift 1";

// --- State Machine ---
enum CycleState { MANUAL,
                  STATE_ON,
                  STATE_WAIT };
CycleState currentState = STATE_ON;

unsigned long previousMillis = 0;
bool pumpState = true;

// --- LittleFS Config Persistence ---
void loadConfiguration() {
  if (!LittleFS.exists("/config.json")) return;

  File configFile = LittleFS.open("/config.json", "r");
  if (!configFile) return;

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, configFile);
  configFile.close();

  if (!error) {
    shift1.startHour = doc["s1H"] | shift1.startHour;
    shift1.durationON = doc["s1On"] | shift1.durationON;
    shift1.durationWAIT = doc["s1Wait"] | shift1.durationWAIT;

    shift2.startHour = doc["s2H"] | shift2.startHour;
    shift2.durationON = doc["s2On"] | shift2.durationON;
    shift2.durationWAIT = doc["s2Wait"] | shift2.durationWAIT;

    shift3.startHour = doc["s3H"] | shift3.startHour;
    shift3.durationON = doc["s3On"] | shift3.durationON;
    shift3.durationWAIT = doc["s3Wait"] | shift3.durationWAIT;
  }
}

void saveConfiguration() {
  JsonDocument doc;

  doc["s1H"] = shift1.startHour;
  doc["s1On"] = shift1.durationON;
  doc["s1Wait"] = shift1.durationWAIT;
  doc["s2H"] = shift2.startHour;
  doc["s2On"] = shift2.durationON;
  doc["s2Wait"] = shift2.durationWAIT;
  doc["s3H"] = shift3.startHour;
  doc["s3On"] = shift3.durationON;
  doc["s3Wait"] = shift3.durationWAIT;

  File configFile = LittleFS.open("/config.json", "w");
  if (configFile) {
    serializeJson(doc, configFile);
    configFile.close();
  }
}

// Update shift profile based on NTP hour
void updateShiftProfile() {
  time_t now = time(nullptr);
  struct tm* timeinfo = localtime(&now);

  if (timeinfo->tm_year > 70) {
    int currentHour = timeinfo->tm_hour;

    if ((shift1.startHour < shift2.startHour && currentHour >= shift1.startHour && currentHour < shift2.startHour) || (shift1.startHour > shift2.startHour && (currentHour >= shift1.startHour || currentHour < shift2.startHour))) {
      activeShiftName = shift1.name;
      durationON = shift1.durationON * 60;
      durationWAIT = shift1.durationWAIT * 60;
    } else if ((shift2.startHour < shift3.startHour && currentHour >= shift2.startHour && currentHour < shift3.startHour) || (shift2.startHour > shift3.startHour && (currentHour >= shift2.startHour || currentHour < shift3.startHour))) {
      activeShiftName = shift2.name;
      durationON = shift2.durationON * 60;
      durationWAIT = shift2.durationWAIT * 60;
    } else {
      activeShiftName = shift3.name;
      durationON = shift3.durationON * 60;
      durationWAIT = shift3.durationWAIT * 60;
    }
  }
}

long getRemainingSeconds() {
  if (currentState == MANUAL) return 0;

  unsigned long currentMillis = millis();
  unsigned long elapsedSec = (currentMillis - previousMillis) / 1000;
  unsigned long targetSec = (currentState == STATE_ON) ? durationON : durationWAIT;

  if (elapsedSec >= targetSec) return 0;
  return targetSec - elapsedSec;
}

// --- Modern Web Interface ---
void handleRoot() {
  updateShiftProfile();
  long remainingSec = getRemainingSeconds();

  time_t now = time(nullptr);
  struct tm* timeinfo = localtime(&now);
  char timeStr[30] = "Syncing NTP Time...";
  if (timeinfo->tm_year > 70) {
    strftime(timeStr, sizeof(timeStr), "%I:%M:%S %p", timeinfo);
  }

  String html = "<!DOCTYPE html><html lang='en'><head><meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  html += "<title>Hydroponics Dashboard</title>";
  html += "<meta http-equiv='refresh' content='4'>";

  html += "<style>";
  html += ":root{--primary:#2563eb;--bg:#f8fafc;--card:#ffffff;--text:#1e293b;}";
  html += "body{font-family:'Segoe UI',Roboto,sans-serif;background:var(--bg);color:var(--text);margin:0;padding:20px;}";
  html += ".container{max-width:480px;margin:0 auto;}";
  html += ".card{background:var(--card);padding:24px;border-radius:16px;box-shadow:0 4px 12px rgba(0,0,0,0.05);margin-bottom:20px;}";
  html += "h2,h3{margin-top:0;text-align:center;color:#0f172a;}";
  html += ".status-badge{display:inline-block;padding:6px 14px;border-radius:20px;font-weight:600;font-size:14px;color:white;margin:5px 0;}";
  html += ".running{background:#16a34a;} .stopped{background:#dc2626;} .shift-tag{background:#0284c7;}";
  html += ".timer-display{font-size:38px;font-weight:700;color:var(--primary);text-align:center;margin:15px 0;letter-spacing:1px;}";
  html += ".btn-group{display:flex;gap:10px;justify-content:center;margin-top:15px;}";
  html += ".btn{flex:1;padding:12px;border:none;border-radius:10px;font-size:15px;font-weight:600;cursor:pointer;transition:0.2s;}";
  html += ".btn-on{background:#22c55e;color:white;} .btn-off{background:#ef4444;color:white;}";
  html += ".shift-row{background:#f1f5f9;padding:12px;border-radius:10px;margin-bottom:10px;text-align:left;}";
  html += ".shift-row label{font-weight:600;display:block;margin-bottom:5px;color:#334155;}";
  html += "input[type=number]{width:55px;padding:6px;border:1px solid #cbd5e1;border-radius:6px;text-align:center;}";
  html += "</style></head><body>";

  html += "<div class='container'>";

  // Card 1: Live Status
  html += "<div class='card'>";
  html += "<h2>Hydro Controller</h2>";
  html += "<div style='text-align:center;'>";
  html += "<p style='margin:4px 0;color:#64748b;'>Current Time: <b>" + String(timeStr) + "</b></p>";
  html += "<span class='status-badge shift-tag'>" + activeShiftName + "</span><br>";
  html += "<span class='status-badge " + String(pumpState ? "running" : "stopped") + "'>" + String(pumpState ? "PUMP ACTIVE" : "PUMP IDLE") + "</span>";
  html += "</div>";

  String stateStr = "MANUAL OVERRIDE";
  if (currentState == STATE_ON) stateStr = "PUMP RUNNING";
  if (currentState == STATE_WAIT) stateStr = "CYCLE WAITING (OFF)";

  html += "<p style='text-align:center;margin-top:15px;font-weight:600;'>" + stateStr + "</p>";

  if (currentState != MANUAL) {
    html += "<div class='timer-display' id='countdown'>" + String(remainingSec / 60) + "m " + String(remainingSec % 60) + "s</div>";
  } else {
    html += "<div class='timer-display'>--:--</div>";
  }

  html += "<div class='btn-group'>";
  html += "<a href='/manual?state=on' style='flex:1;'><button class='btn btn-on'>Manual ON</button></a>";
  html += "<a href='/manual?state=off' style='flex:1;'><button class='btn btn-off'>Manual OFF</button></a>";
  html += "</div></div>";

  // Card 2: Shift Configuration
  html += "<div class='card'>";
  html += "<h3>Shift Schedules</h3>";
  html += "<form action='/setTimer' method='GET'>";

  // Shift 1
  html += "<div class='shift-row'><label>Shift 1 Start Hour (24h): <input type='number' name='s1H' value='" + String(shift1.startHour) + "' min='0' max='23'></label>";
  html += "ON: <input type='number' name='s1On' value='" + String(shift1.durationON) + "'>m ";
  html += "WAIT (OFF): <input type='number' name='s1Wait' value='" + String(shift1.durationWAIT) + "'>m</div>";

  // Shift 2
  html += "<div class='shift-row'><label>Shift 2 Start Hour (24h): <input type='number' name='s2H' value='" + String(shift2.startHour) + "' min='0' max='23'></label>";
  html += "ON: <input type='number' name='s2On' value='" + String(shift2.durationON) + "'>m ";
  html += "WAIT (OFF): <input type='number' name='s2Wait' value='" + String(shift2.durationWAIT) + "'>m</div>";

  // Shift 3
  html += "<div class='shift-row'><label>Shift 3 Start Hour (24h): <input type='number' name='s3H' value='" + String(shift3.startHour) + "' min='0' max='23'></label>";
  html += "ON: <input type='number' name='s3On' value='" + String(shift3.durationON) + "'>m ";
  html += "WAIT (OFF): <input type='number' name='s3Wait' value='" + String(shift3.durationWAIT) + "'>m</div>";

  html += "<div class='btn-group' style='margin-top:15px;'>";
  html += "<input type='submit' name='mode' value='Save Settings' class='btn btn-on'>";
  html += "<input type='submit' name='mode' value='Stop Cycle' class='btn btn-off'>";
  html += "</div></form></div></div>";

  // JS Countdown
  html += "<script>";
  html += "var timeLeft = " + String(remainingSec) + ";";
  html += "if(timeLeft > 0) {";
  html += "  setInterval(function(){";
  html += "    if(timeLeft <= 0) return;";
  html += "    timeLeft--;";
  html += "    var m = Math.floor(timeLeft / 60);";
  html += "    var s = timeLeft % 60;";
  html += "    document.getElementById('countdown').innerHTML = m + 'm ' + s + 's';";
  html += "  }, 1000);";
  html += "}";
  html += "</script></body></html>";

  server.send(200, "text/html", html);
}

void handleManual() {
  if (server.hasArg("state")) {
    String state = server.arg("state");
    currentState = MANUAL;
    if (state == "on") {
      digitalWrite(RELAY_PIN, PUMP_ON);
      pumpState = true;
    } else {
      digitalWrite(RELAY_PIN, PUMP_OFF);
      pumpState = false;
    }
  }
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleSetTimer() {
  if (server.hasArg("s1H")) shift1.startHour = server.arg("s1H").toInt();
  if (server.hasArg("s1On")) shift1.durationON = server.arg("s1On").toInt();
  if (server.hasArg("s1Wait")) shift1.durationWAIT = server.arg("s1Wait").toInt();

  if (server.hasArg("s2H")) shift2.startHour = server.arg("s2H").toInt();
  if (server.hasArg("s2On")) shift2.durationON = server.arg("s2On").toInt();
  if (server.hasArg("s2Wait")) shift2.durationWAIT = server.arg("s2Wait").toInt();

  if (server.hasArg("s3H")) shift3.startHour = server.arg("s3H").toInt();
  if (server.hasArg("s3On")) shift3.durationON = server.arg("s3On").toInt();
  if (server.hasArg("s3Wait")) shift3.durationWAIT = server.arg("s3Wait").toInt();

  if (server.hasArg("mode")) {
    String mode = server.arg("mode");
    if (mode == "Save Settings") {
      saveConfiguration();
      updateShiftProfile();
      currentState = STATE_ON;
      digitalWrite(RELAY_PIN, PUMP_ON);
      pumpState = true;
      previousMillis = millis();
    } else {
      currentState = MANUAL;
      digitalWrite(RELAY_PIN, PUMP_OFF);
      pumpState = false;
    }
  }
  server.sendHeader("Location", "/");
  server.send(303);
}

void setup() {
  Serial.begin(115200);
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, PUMP_ON);
  previousMillis = millis();

  String chipId = String(ESP.getChipId());
  String uniqueHostname = "hydro-pump-" + chipId;
  String uniqueAPName = "Hydro-Setup-" + chipId;

  WiFi.hostname(uniqueHostname);

  // 1. Initialize LittleFS Flash Storage
  if (LittleFS.begin()) {
    loadConfiguration();
  }

  // 2. Configure WiFiManager
  WiFiManager wifiManager;
  wifiManager.setSTAStaticIPConfig(local_IP, gateway, subnet, primaryDNS);
  // Debugging Only [Remove before deployment]
  //wifiManager.resetSettings();

  if (!wifiManager.autoConnect(uniqueAPName.c_str())) {
    Serial.println("Failed to connect, resetting...");
    ESP.restart();
  }

  // 3. NTP & ArduinoOTA Setup
  configTime(MY_TZ, ntpServer);
  ArduinoOTA.setHostname(uniqueHostname.c_str());
  ArduinoOTA.setPassword("theoretics1711");
  ArduinoOTA.begin();

  // 4. Start Server
  server.on("/", handleRoot);
  server.on("/manual", handleManual);
  server.on("/setTimer", handleSetTimer);
  server.begin();
}

void loop() {
  ArduinoOTA.handle();
  server.handleClient();

  if (currentState != MANUAL) {
    unsigned long currentMillis = millis();

    switch (currentState) {
      case STATE_ON:
        if (currentMillis - previousMillis >= (durationON * 1000)) {
          currentState = STATE_WAIT;
          digitalWrite(RELAY_PIN, PUMP_OFF);
          pumpState = false;
          previousMillis = currentMillis;
        }
        break;

      case STATE_WAIT:
        if (currentMillis - previousMillis >= (durationWAIT * 1000)) {
          updateShiftProfile();
          currentState = STATE_ON;
          digitalWrite(RELAY_PIN, PUMP_ON);
          pumpState = true;
          previousMillis = currentMillis;
        }
        break;

      default:
        break;
    }
  }
}