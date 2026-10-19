#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <DNSServer.h>
#include <WiFiManager.h>  // https://github.com/tzapu/WiFiManager
#include <LittleFS.h>
#include <WiFiClient.h>
#include <ESP8266HTTPClient.h>
#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include <time.h>

// =========================================================================
//  SYSTEM SELECTION (Select "AERO" or "HYDRO")
// =========================================================================
const String SYSTEM_TYPE = "HYDRO";  // Options: "AERO" or "HYDRO"

// Pin & System Config set dynamically in setup() based on SYSTEM_TYPE
int RELAY_PIN = D2;                 // Default: D2 for Aero, D5 for Hydro
String systemName = "Hydroponics";  // Display name for web UI  Options "Hydroponics" or "Aeroponics"
String uniqueHostname = "";
String uniqueAPName = "";

#define PUMP_ON LOW
#define PUMP_OFF HIGH

// --- Static IP Settings ---
IPAddress local_IP(192, 168, 1, 100);
IPAddress gateway(192, 168, 1, 1);
IPAddress subnet(255, 255, 255, 0);
IPAddress primaryDNS(8, 8, 8, 8);

// --- OpenWeatherMap Settings ---
const String apiKey = "2d25cc59c11a81fdfee74fcb94daf43c";  // Replace with your key
const String city = "Paranaque";
const String country = "PH";
const double lat = 14.4872085;
const double lon = 121.0231461;

// --- Dynamic Threshold Variables (Configurable via UI) ---
float tempThreshold = 32.0;      // Trigger override if temp >= this value (°C)
float cloudThreshold = 70.0;     // Prevent override if cloudiness >= this value (%)
float humidityThreshold = 60.0;  // Percentage (0 - 100%)

// --- Weather & Override Constants ---
const unsigned long WEATHER_CHECK_INTERVAL = 10 * 60 * 1000UL;  // Fetch weather every 10 mins
const unsigned long CYCLE_PERIOD = 5 * 60 * 1000UL;             // 5-minute total cycle
const unsigned long ON_DURATION = 2 * 60 * 1000UL;              // 2 minutes ON

// --- State Variables ---
unsigned long lastWeatherCheck = 0;
unsigned long cycleStartTime = 0;
float currentTempC = 0.0;
float currentClouds = 0.0;
float currentHumidity = 0.0;

bool overrideActive = false;       // Master flag indicating whether temp override cycle is active
bool manualOverrideForce = false;  // Set to true if overridden via Web UI

// Function declarations
void fetchTemperature();
void handleCycleTiming();

// --- Time Zone / NTP Settings ---
#define MY_TZ "PHT-8"
const char* ntpServer = "pool.ntp.org";

ESP8266WebServer server(80);

// --- Shift Structures ---
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

    // Load weather thresholds
    tempThreshold = doc["tThresh"] | tempThreshold;
    cloudThreshold = doc["cThresh"] | cloudThreshold;
    humidityThreshold = doc["hThresh"] | humidityThreshold;
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

  // Save weather thresholds
  doc["tThresh"] = tempThreshold;
  doc["cThresh"] = cloudThreshold;
  doc["hThresh"] = humidityThreshold;

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
  html += "<title>" + systemName + " Dashboard</title>";
  html += "<meta http-equiv='refresh' content='4'>";

  html += "<style>";
  html += ":root{--primary:#2563eb;--bg:#f8fafc;--card:#ffffff;--text:#1e293b;}";
  html += "body{font-family:'Segoe UI',Roboto,sans-serif;background:var(--bg);color:var(--text);margin:0;padding:20px;}";
  html += ".container{max-width:480px;margin:0 auto;}";
  html += ".card{background:var(--card);padding:24px;border-radius:16px;box-shadow:0 4px 12px rgba(0,0,0,0.05);margin-bottom:20px;}";
  html += "h2,h3{margin-top:0;text-align:center;color:#0f172a;}";
  html += ".status-badge{display:inline-block;padding:6px 14px;border-radius:20px;font-weight:600;font-size:14px;color:white;margin:5px 0;}";
  html += ".running{background:#16a34a;} .stopped{background:#dc2626;} .shift-tag{background:#0284c7;} .override-tag{background:#d97706;}";
  html += ".timer-display{font-size:38px;font-weight:700;color:var(--primary);text-align:center;margin:15px 0;letter-spacing:1px;}";
  html += ".btn-group{display:flex;gap:10px;justify-content:center;margin-top:15px;}";
  html += ".btn{flex:1;padding:12px;border:none;border-radius:10px;font-size:14px;font-weight:600;cursor:pointer;transition:0.2s;text-align:center;text-decoration:none;display:inline-block;}";
  html += ".btn-on{background:#22c55e;color:white;} .btn-off{background:#ef4444;color:white;} .btn-warn{background:#f59e0b;color:white;} .btn-neutral{background:#64748b;color:white;}";
  html += ".shift-row{background:#f1f5f9;padding:12px;border-radius:10px;margin-bottom:10px;text-align:left;}";
  html += ".shift-row label{font-weight:600;display:block;margin-bottom:5px;color:#334155;}";
  html += "input[type=number]{width:60px;padding:6px;border:1px solid #cbd5e1;border-radius:6px;text-align:center;}";
  html += "</style></head><body>";

  html += "<div class='container'>";

  // Card 1: Live Status
  html += "<div class='card'>";
  html += "<h2>" + systemName + " Controller</h2>";
  html += "<div style='text-align:center;'>";
  html += "<p style='margin:4px 0;color:#64748b;'>Time: <b>" + String(timeStr) + "</b></p>";
  html += "<p style='margin:4px 0;color:#64748b;'>Parañaque Temp: <b>" + String(currentTempC, 1) + " °C</b></p>";
  html += "<p style='margin:4px 0;color:#64748b;'>Clouds: <b>" + String(currentClouds, 0) + " %</b></p>";
  html += "<p style='margin:4px 0;color:#64748b;'>Humidity: <b>" + String(currentHumidity, 0) + " %</b></p>";

  if (overrideActive) {
    html += "<span class='status-badge override-tag'>OVERRIDE ACTIVE (2m ON / 3m OFF)</span><br>";
  } else {
    html += "<span class='status-badge shift-tag'>" + activeShiftName + "</span><br>";
  }

  html += "<span class='status-badge " + String(pumpState ? "running" : "stopped") + "'>" + String(pumpState ? "PUMP ACTIVE" : "PUMP IDLE") + "</span>";
  html += "</div>";

  String stateStr = "MANUAL PUMP OVERRIDE";
  if (overrideActive) {
    stateStr = "2m ON / 3m OFF OVERRIDE CYCLE";
  } else if (currentState == STATE_ON) {
    stateStr = "PUMP RUNNING (SHIFT)";
  } else if (currentState == STATE_WAIT) {
    stateStr = "CYCLE WAITING (OFF)";
  }

  html += "<p style='text-align:center;margin-top:15px;font-weight:600;'>" + stateStr + "</p>";

  if (!overrideActive && currentState != MANUAL) {
    html += "<div class='timer-display' id='countdown'>" + String(remainingSec / 60) + "m " + String(remainingSec % 60) + "s</div>";
  } else {
    html += "<div class='timer-display'>--:--</div>";
  }

  // Direct Manual Control Buttons
  html += "<div class='btn-group'>";
  html += "<a href='/manual?state=on' class='btn btn-on'>Manual ON</a>";
  html += "<a href='/manual?state=off' class='btn btn-off'>Manual OFF</a>";
  html += "</div>";

  // Temperature Override Toggle Controls
  html += "<div style='margin-top:18px; padding-top:12px; border-top:1px solid #e2e8f0; text-align:center;'>";
  html += "<p style='margin:0 0 8px 0; font-size:13px; font-weight:600; color:#475569;'>Temp Override Cycle (2m ON / 3m OFF)</p>";
  html += "<div class='btn-group'>";
  html += "<a href='/toggleOverride?enable=1' class='btn btn-warn'>Enable Override</a>";
  html += "<a href='/toggleOverride?enable=0' class='btn btn-neutral'>Disable Override</a>";
  html += "</div></div>";

  html += "</div>";

  // Card 2: Weather Threshold Settings
  html += "<div class='card'>";
  html += "<h3>Weather Trigger Settings</h3>";
  html += "<form action='/setThresholds' method='GET'>";
  html += "<div class='shift-row'><label>Temp Threshold (°C): <input type='number' step='0.5' name='tThresh' value='" + String(tempThreshold, 1) + "'></label>";
  html += "<span style='font-size:12px;color:#64748b;'>Trigger override if temp is at or above this value.</span></div>";

  html += "<div class='shift-row'><label>Cloudiness Cap (%): <input type='number' name='cThresh' value='" + String(cloudThreshold, 0) + "' min='0' max='100'></label>";
  html += "<span style='font-size:12px;color:#64748b;'>Block override if clouds exceed this percentage.</span></div>";

  html += "<div class='shift-row'><label>Humidity (%): <input type='number' name='hThresh' value='" + String(humidityThreshold, 0) + "' min='0' max='100'></label>";
  html += "<span style='font-size:12px;color:#64748b;'>Block override if humidity exceed this percentage.</span></div>";

  html += "<div class='btn-group' style='margin-top:10px;'>";
  html += "<input type='submit' value='Save Thresholds' class='btn btn-on'>";
  html += "</div></form></div>";

  // Card 3: Shift Configuration
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
    overrideActive = false;
    manualOverrideForce = false;
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

void handleToggleOverride() {
  if (server.hasArg("enable")) {
    int enableVal = server.arg("enable").toInt();
    if (enableVal == 1) {
      manualOverrideForce = true;
      overrideActive = true;
      cycleStartTime = millis();
    } else {
      manualOverrideForce = false;
      overrideActive = false;
      currentState = STATE_ON;
      previousMillis = millis();
    }
  }
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleSetThresholds() {
  if (server.hasArg("tThresh")) tempThreshold = server.arg("tThresh").toFloat();
  if (server.hasArg("cThresh")) cloudThreshold = server.arg("cThresh").toFloat();
  if (server.hasArg("hThresh")) humidityThreshold = server.arg("hThresh").toFloat();

  saveConfiguration();
  fetchTemperature();  // Re-evaluate weather immediately with new thresholds

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

void fetchTemperature() {
  if (WiFi.status() == WL_CONNECTED) {
    WiFiClient client;
    HTTPClient http;

    //String url = "http://api.openweathermap.org/data/2.5/weather?q=" + city + "," + country + "&units=metric&appid=" + apiKey;

    //String url = "https://api.openweathermap.org/data/2.5/weather?lat=" + String(lat, 4) + "&lon=" + String(lon, 4) + "&units=metric&lang=en&appid=" + String(apiKey);

    char urlBuffer[256];
    snprintf(urlBuffer, sizeof(urlBuffer),
             "https://api.openweathermap.org/data/2.5/weather?lat=%.4f&lon=%.4f&units=metric&lang=en&appid=%s",
             lat, lon, apiKey);

    String url = String(urlBuffer);

    http.begin(client, url);
    int httpCode = http.GET();

    if (httpCode == HTTP_CODE_OK) {
      String payload = http.getString();

      JsonDocument doc;
      DeserializationError error = deserializeJson(doc, payload);

      if (!error) {
        currentTempC = doc["main"]["temp"];
        currentClouds = doc["clouds"]["all"];
        currentHumidity = doc["main"]["humidity"];                                   // Percentage (0 - 100%)
                                                                                     // Approximate VPD calculation in Celsius
        float svp = 0.61078 * exp((17.27 * currentTempC) / (currentTempC + 237.3));  // Saturation Vapor Pressure (kPa)
        float avp = svp * (currentHumidity / 100.0);                                 // Actual Vapor Pressure (kPa)
        float vpd = svp - avp;                                                       // Vapor Pressure Deficit (kPa)

        Serial.printf("Current Parañaque Temp: %.2f °C | Clouds: %.0f %% | Humiditys: %.0f %%\n", currentTempC, currentClouds, currentHumidity);

        // Evaluate weather condition if manual force is not active
        if (!manualOverrideForce) {
          // Trigger override ONLY if temp is AT OR ABOVE tempThreshold AND cloudiness IS BELOW cloudThreshold
          //if (currentTempC >= tempThreshold && currentClouds < cloudThreshold && currentHumidity < humidityThreshold) {
          if (vpd > 1.5 || (currentTempC >= tempThreshold && currentClouds < cloudThreshold)) {
            if (!overrideActive) cycleStartTime = millis();
            overrideActive = true;
          } else {
            overrideActive = false;
          }
        }
      } else {
        Serial.print("JSON Parse Error: ");
        Serial.println(error.c_str());
      }
    } else {
      Serial.printf("HTTP GET failed, error code: %d\n", httpCode);
    }
    http.end();
  }
}

void handleCycleTiming() {
  if (!overrideActive || currentState == MANUAL) return;

  unsigned long currentMillis = millis();
  unsigned long timeInCycle = (currentMillis - cycleStartTime) % CYCLE_PERIOD;

  if (timeInCycle < ON_DURATION) {
    digitalWrite(RELAY_PIN, PUMP_ON);
    pumpState = true;
  } else {
    digitalWrite(RELAY_PIN, PUMP_OFF);
    pumpState = false;
  }
}

void setup() {
  Serial.begin(115200);

  // Configure hardware and names dynamically based on SYSTEM_TYPE
  String chipId = String(ESP.getChipId());
  if (SYSTEM_TYPE == "HYDRO") {
    RELAY_PIN = D5;
    systemName = "Hydroponics";
    uniqueHostname = "hydro-pump-" + chipId;
    uniqueAPName = "Hydro-Setup-" + chipId;
    local_IP = IPAddress(192, 168, 1, 120);  // IP .120 for Hydroponics
  } else {
    RELAY_PIN = D2;
    systemName = "Aeroponics";
    uniqueHostname = "aero-pump-" + chipId;
    uniqueAPName = "Aero-Setup-" + chipId;
    local_IP = IPAddress(192, 168, 1, 110);  // IP .110 for Aeroponics
  }

  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, PUMP_ON);
  previousMillis = millis();

  // 1. Initialize LittleFS Storage & Load Config
  if (LittleFS.begin()) {
    loadConfiguration();
  }

  // 2. Configure WiFiManager
  WiFiManager wifiManager;

  //FOR Debug Only
  //wifiManager.resetSettings();

  WiFi.mode(WIFI_STA);
  wifiManager.setSTAStaticIPConfig(local_IP, gateway, subnet, primaryDNS);

  if (!wifiManager.autoConnect(uniqueAPName.c_str())) {
    Serial.println("Failed to connect, resetting...");
    ESP.restart();
  }

  WiFi.hostname(uniqueHostname);

  // 3. NTP & ArduinoOTA Setup
  configTime(MY_TZ, ntpServer);
  ArduinoOTA.setHostname(uniqueHostname.c_str());
  ArduinoOTA.setPassword("theoretics1711");
  ArduinoOTA.begin();

  // 4. Initial Weather Check
  fetchTemperature();
  lastWeatherCheck = millis();

  // 5. Start Web Server
  server.on("/", handleRoot);
  server.on("/manual", handleManual);
  server.on("/toggleOverride", handleToggleOverride);
  server.on("/setThresholds", handleSetThresholds);
  server.on("/setTimer", handleSetTimer);
  server.begin();
}

void loop() {
  MDNS.update();
  ArduinoOTA.handle();
  server.handleClient();

  // 1. Periodically check ambient temperature online
  if (millis() - lastWeatherCheck >= WEATHER_CHECK_INTERVAL) {
    fetchTemperature();
    lastWeatherCheck = millis();
  }

  // 2. Main Relay Control Dispatcher
  if (overrideActive && currentState != MANUAL) {
    // 2m ON / 3m OFF override cycle
    handleCycleTiming();
  } else if (currentState != MANUAL) {
    // Normal Shift Timing Logic
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