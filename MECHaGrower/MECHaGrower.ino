#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <EEPROM.h>
#include <time.h>

// --- WiFi Credentials (Home Router) ---
const char* ssid = "PLDTHOMEFIBR2XamC_2.4G";
const char* password = "PLDTWIFI6UnMM";

// --- Direct Access Point (AP) Settings ---
const char* ap_ssid = "MECHaGrower";
const char* ap_password = "";

// --- Static IP Settings (Home Network) ---
IPAddress local_IP(192, 168, 1, 115);
IPAddress gateway(192, 168, 1, 1);     
IPAddress subnet(255, 255, 255, 0);
IPAddress primaryDNS(8, 8, 8, 8);      

// --- Time & NTP Settings ---
const long gmtOffset_sec = 28800; // GMT+8
const int daylightOffset_sec = 0;
const char* ntpServer = "pool.ntp.org";

// --- Pin Definitions ---
const int lightRelayPin = D1; // Grow Lights Relay
const int fanRelayPin =   D5; // 12V Fan Relay

// --- Settings Structure (Saved in EEPROM) ---
struct Settings {
  int lightStartHour = 18;      
  int lightEndHour = 2;         
  int lightOnMinutes = 45;      
  int lightOffMinutes = 15;     
  
  int fanOnSeconds = 30;        
  int fanOffMinutes = 30;       
  int fanStartHour = 6;         
  int fanEndHour = 22;          
  
  char owmApiKey[33] = "2d25cc59c11a81fdfee74fcb94daf43c";      
  char owmCity[32] = "Paranaque";  
  float windThreshold = 5.0;    
};
Settings config;

// --- Manual Override States ---
// 0 = Auto, 1 = Forced ON, 2 = Forced OFF
int lightManualMode = 0; 
int fanManualMode = 0;

// --- Timing State Variables ---
unsigned long lightPreviousMillis = 0;
bool lightStateActive = true;         
unsigned long secondsUntilLightToggle = 0; 

unsigned long fanPreviousMillis = 0;
bool fanStateActive = true;           
unsigned long secondsUntilFanOn = 0;  

// --- Weather Check Variables ---
unsigned long lastWeatherCheck = 0;
const unsigned long WEATHER_CHECK_INTERVAL = 15UL * 60UL * 1000UL; // 15 mins
float currentWindSpeed = 0.0;
bool isWindyOverride = false;

ESP8266WebServer server(80);

void saveConfig() {
  EEPROM.begin(512);
  EEPROM.put(0, config);
  EEPROM.commit();
  EEPROM.end();
}

void loadConfig() {
  EEPROM.begin(512);
  EEPROM.get(0, config);
  EEPROM.end();
  
  if (config.lightStartHour < 0 || config.lightStartHour > 23) config.lightStartHour = 18;
  if (config.lightEndHour < 0 || config.lightEndHour > 23) config.lightEndHour = 2;
  if (config.lightOnMinutes < 1 || config.lightOnMinutes > 1440) config.lightOnMinutes = 45;
  if (config.lightOffMinutes < 1 || config.lightOffMinutes > 1440) config.lightOffMinutes = 15;
  
  if (config.fanOnSeconds < 1 || config.fanOnSeconds > 3600) config.fanOnSeconds = 30; 
  if (config.fanOffMinutes < 1 || config.fanOffMinutes > 1440) config.fanOffMinutes = 30;
  if (config.fanStartHour < 0 || config.fanStartHour > 23) config.fanStartHour = 6;
  if (config.fanEndHour < 0 || config.fanEndHour > 23) config.fanEndHour = 22;
  if (config.windThreshold <= 0.0 || config.windThreshold > 50.0) config.windThreshold = 5.0;
}

// --- Check OpenWeatherMap for Wind ---
void checkOpenWeatherMap() {
  if (strlen(config.owmApiKey) == 0) return;
  
  if (WiFi.status() == WL_CONNECTED) {
    WiFiClient client;
    HTTPClient http;
    
    String url = "http://api.openweathermap.org/data/2.5/weather?q=" + String(config.owmCity) + "&appid=" + String(config.owmApiKey) + "&units=metric";
    
    if (http.begin(client, url)) {
      int httpCode = http.GET();
      if (httpCode > 0 && httpCode == HTTP_CODE_OK) {
        String payload = http.getString();
        int windIndex = payload.indexOf("\"wind\":");
        if (windIndex != -1) {
          int speedIndex = payload.indexOf("\"speed\":", windIndex);
          if (speedIndex != -1) {
            int startVal = speedIndex + 8;
            int endVal = payload.indexOf(",", startVal);
            if (endVal == -1) endVal = payload.indexOf("}", startVal);
            currentWindSpeed = payload.substring(startVal, endVal).toFloat();
            isWindyOverride = (currentWindSpeed >= config.windThreshold);
          }
        }
      }
      http.end();
    }
  }
}

// --- Modern Web Dashboard HTML ---
void handleRoot() {
  time_t now;
  struct tm* timeinfo;
  time(&now);
  timeinfo = localtime(&now);
  
  char currentTimeStr[32];
  sprintf(currentTimeStr, "%02d:%02d:%02d", timeinfo->tm_hour, timeinfo->tm_min, timeinfo->tm_sec);

  String html = "<!DOCTYPE html><html lang='en'><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<title>MECHaGrower Controller</title>";
  html += "<style>";
  html += ":root { --primary: #2ecc71; --primary-dark: #27ae60; --bg: #f8f9fa; --card-bg: #ffffff; --text: #2c3e50; --gray: #bdc3c7; }";
  html += "body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, Helvetica, Arial, sans-serif; background: var(--bg); color: var(--text); margin: 0; padding: 20px; }";
  html += ".container { max-width: 500px; margin: auto; }";
  html += "h1 { text-align: center; color: var(--primary-dark); font-size: 1.8rem; margin-bottom: 20px; }";
  html += ".card { background: var(--card-bg); padding: 20px; border-radius: 12px; box-shadow: 0 4px 15px rgba(0,0,0,0.05); margin-bottom: 20px; }";
  html += "h3 { margin-top: 0; color: #34495e; border-bottom: 2px solid #f1f2f6; padding-bottom: 8px; font-size: 1.1rem; }";
  html += "label { display: block; margin-top: 12px; font-weight: 600; font-size: 0.9rem; color: #576574; }";
  html += "input[type='number'], input[type='text'] { width: 100%; padding: 10px; margin-top: 6px; box-sizing: border-box; border: 1px solid #dcdde1; border-radius: 6px; font-size: 1rem; background: #fdfdfd; }";
  html += "input:focus { border-color: var(--primary); outline: none; box-shadow: 0 0 0 3px rgba(46,204,113,0.15); }";
  html += ".btn { display: inline-block; padding: 8px 14px; margin: 6px 4px 0 0; border: none; border-radius: 6px; color: white; text-decoration: none; font-size: 0.9rem; font-weight: 600; cursor: pointer; transition: opacity 0.2s; }";
  html += ".btn:hover { opacity: 0.9; }";
  html += ".btn-auto { background: #3498db; }";
  html += ".btn-on { background: #2ecc71; }";
  html += ".btn-off { background: #e74c3c; }";
  html += ".btn-save { background: #e67e22; width: 100%; margin-top: 20px; padding: 12px; font-size: 1rem; }";
  html += ".badge { display: inline-block; padding: 4px 8px; border-radius: 4px; font-size: 0.85rem; font-weight: bold; background: #dfe4ea; color: #2f3640; }";
  html += ".status-box { background: #f1f2f6; padding: 12px; border-radius: 8px; margin-bottom: 15px; font-size: 0.95rem; line-height: 1.5; }";
  html += ".alert-wind { background: #ff7675; color: white; padding: 10px; border-radius: 6px; margin-top: 10px; font-weight: bold; font-size: 0.9rem; text-align: center; }";
  html += "</style>";
  
  html += "<script>";
  html += "let isWindy = " + String(isWindyOverride ? "true" : "false") + ";";
  html += "let fanRemaining = " + String(secondsUntilFanOn) + ";";
  html += "let lightRemaining = " + String(secondsUntilLightToggle) + ";";
  
  html += "function formatTime(secs) {";
  html += "  if (secs <= 0) return 'Toggling now...';";
  html += "  let hrs = Math.floor(secs / 3600);";
  html += "  let mins = Math.floor((secs % 3600) / 60);";
  html += "  let s = secs % 60;";
  html += "  return (hrs > 0 ? hrs + 'h ' : '') + (mins > 0 ? mins + 'm ' : '') + s + 's';";
  html += "}";

  html += "function updateCountdowns() {";
  html += "  let fanEl = document.getElementById('fan-countdown');";
  html += "  if (fanEl) {";
  html += "    if (isWindy) { fanEl.innerHTML = '<span style=\"color:red;\">Paused (High Wind)</span>'; }";
  html += "    else { fanEl.innerHTML = formatTime(fanRemaining); fanRemaining = Math.max(0, fanRemaining - 1); }";
  html += "  }";
  
  html += "  let lightEl = document.getElementById('light-countdown');";
  html += "  if (lightEl) {";
  html += "    lightEl.innerHTML = formatTime(lightRemaining);";
  html += "    lightRemaining = Math.max(0, lightRemaining - 1);";
  html += "  }";
  html += "}";
  
  html += "setInterval(updateCountdowns, 1000);";
  html += "setTimeout(function(){ location.reload(); }, 10000);";
  html += "</script>";
  
  html += "</head><body>";
  html += "<div class='container'><h1>🌿 MECHaGrower Dashboard</h1>";
  
  // Status Card
  html += "<div class='card'>";
  html += "<h3>System Status</h3>";
  html += "<div class='status-box'>";
  html += "🕒 <b>NTP Time:</b> " + String(currentTimeStr) + "<br>";
  html += "💨 <b>Wind Speed:</b> " + String(currentWindSpeed) + " m/s (Threshold: " + String(config.windThreshold) + " m/s)<br>";
  html += "💡 <b>Lights Toggle in:</b> <span id='light-countdown' style='font-weight:bold; color:#27ae60;'>Calculating...</span><br>";
  html += "⏳ <b>Next Fan ON in:</b> <span id='fan-countdown' style='font-weight:bold; color:#2980b9;'>Calculating...</span>";
  html += "</div>";
  if (isWindyOverride) {
    html += "<div class='alert-wind'>⚠️ HIGH WIND DETECTED: Fan Forced OFF</div>";
  }
  html += "</div>";
  
  // Controls Card
  html += "<div class='card'>";
  html += "<h3>Manual Overrides</h3>";
  
  html += "<label>Grow Lights Mode: <span class='badge'>" + String(lightManualMode == 0 ? "Auto" : (lightManualMode == 1 ? "Forced ON" : "Forced OFF")) + "</span></label>";
  html += "<a href='/light?action=auto' class='btn btn-auto'>Auto</a>";
  html += "<a href='/light?action=on' class='btn btn-on'>Turn ON</a>";
  html += "<a href='/light?action=off' class='btn btn-off'>Turn OFF</a>";

  html += "<label style='margin-top:15px;'>12V Fan Mode: <span class='badge'>" + String(fanManualMode == 0 ? "Auto" : (fanManualMode == 1 ? "Forced ON" : "Forced OFF")) + "</span></label>";
  html += "<a href='/fan?action=auto' class='btn btn-auto'>Auto</a>";
  html += "<a href='/fan?action=on' class='btn btn-on'>Turn ON</a>";
  html += "<a href='/fan?action=off' class='btn btn-off'>Turn OFF</a>";
  html += "</div>";

  // Settings Form Card
  html += "<div class='card'>";
  html += "<form action='/save' method='POST'>";
  
  html += "<h3>Grow Lights Intermittent Settings</h3>";
  html += "<label>Active Range Start Hour (0-23):</label>";
  html += "<input type='number' name='lightStart' min='0' max='23' value='" + String(config.lightStartHour) + "'>";
  html += "<label>Active Range End Hour (0-23):</label>";
  html += "<input type='number' name='lightEnd' min='0' max='23' value='" + String(config.lightEndHour) + "'>";
  html += "<label>ON Duration (Minutes):</label>";
  html += "<input type='number' name='lightOnMin' min='1' value='" + String(config.lightOnMinutes) + "'>";
  html += "<label>OFF Rest Duration (Minutes):</label>";
  html += "<input type='number' name='lightOffMin' min='1' value='" + String(config.lightOffMinutes) + "'>";

  html += "<h3 style='margin-top:20px;'>Fan Intermittent Settings</h3>";
  html += "<label>ON Duration (Seconds):</label>";
  html += "<input type='number' name='fanSec' min='1' value='" + String(config.fanOnSeconds) + "'>";
  html += "<label>OFF Wait Time (Minutes):</label>";
  html += "<input type='number' name='fanMin' min='1' value='" + String(config.fanOffMinutes) + "'>";
  html += "<label>Active Range Start Hour (0-23):</label>";
  html += "<input type='number' name='fanStart' min='0' max='23' value='" + String(config.fanStartHour) + "'>";
  html += "<label>Active Range End Hour (0-23):</label>";
  html += "<input type='number' name='fanEnd' min='0' max='23' value='" + String(config.fanEndHour) + "'>";

  html += "<h3 style='margin-top:20px;'>OpenWeatherMap Settings</h3>";
  html += "<label>API Key:</label>";
  html += "<input type='text' name='owmKey' value='" + String(config.owmApiKey) + "'>";
  html += "<label>City Name:</label>";
  html += "<input type='text' name='owmCity' value='" + String(config.owmCity) + "'>";
  html += "<label>Wind Speed Threshold (m/s) to block fan:</label>";
  html += "<input type='number' step='0.1' name='windThresh' value='" + String(config.windThreshold) + "'>";

  html += "<input type='submit' value='Save All Settings' class='btn btn-save'>";
  html += "</form>";
  html += "</div>";
  
  html += "</div></body></html>";
  
  server.send(200, "text/html", html);
}

void handleSave() {
  if (server.hasArg("lightStart")) config.lightStartHour = server.arg("lightStart").toInt();
  if (server.hasArg("lightEnd")) config.lightEndHour = server.arg("lightEnd").toInt();
  if (server.hasArg("lightOnMin")) config.lightOnMinutes = server.arg("lightOnMin").toInt();
  if (server.hasArg("lightOffMin")) config.lightOffMinutes = server.arg("lightOffMin").toInt();
  
  if (server.hasArg("fanSec")) config.fanOnSeconds = server.arg("fanSec").toInt();
  if (server.hasArg("fanMin")) config.fanOffMinutes = server.arg("fanMin").toInt();
  if (server.hasArg("fanStart")) config.fanStartHour = server.arg("fanStart").toInt();
  if (server.hasArg("fanEnd")) config.fanEndHour = server.arg("fanEnd").toInt();
  
  if (server.hasArg("owmKey")) server.arg("owmKey").toCharArray(config.owmApiKey, sizeof(config.owmApiKey));
  if (server.hasArg("owmCity")) server.arg("owmCity").toCharArray(config.owmCity, sizeof(config.owmCity));
  
  if (server.hasArg("windThresh")) {
    config.windThreshold = server.arg("windThresh").toFloat();
  }
  
  saveConfig();

  isWindyOverride = (currentWindSpeed >= config.windThreshold);

  lightStateActive = true;
  lightPreviousMillis = millis();
  fanStateActive = true;
  fanPreviousMillis = millis();

  server.sendHeader("Location", "/", true);
  server.send(303, "text/plain", "Saved! Redirecting...");
}

void handleLightControl() {
  if (server.hasArg("action")) {
    String action = server.arg("action");
    if (action == "auto") lightManualMode = 0;
    else if (action == "on") lightManualMode = 1;
    else if (action == "off") lightManualMode = 2;
  }
  server.sendHeader("Location", "/", true);
  server.send(303, "text/plain", "Updated! Redirecting...");
}

void handleFanControl() {
  if (server.hasArg("action")) {
    String action = server.arg("action");
    if (action == "auto") fanManualMode = 0;
    else if (action == "on") fanManualMode = 1;
    else if (action == "off") fanManualMode = 2;
  }
  server.sendHeader("Location", "/", true);
  server.send(303, "text/plain", "Updated! Redirecting...");
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  pinMode(lightRelayPin, OUTPUT);
  pinMode(fanRelayPin, OUTPUT);
  
  // Safe initial boot state (Relays OFF)
  digitalWrite(lightRelayPin, LOW);
  digitalWrite(fanRelayPin, LOW);

  loadConfig();

  lightStateActive = true;
  lightPreviousMillis = millis();
  fanStateActive = true;
  fanPreviousMillis = millis();

  // --- SET DUAL MODE (AP + STA) ---
  WiFi.mode(WIFI_AP_STA);

  // 1. Configure Home Static IP (STA)
  if (!WiFi.config(local_IP, gateway, subnet, primaryDNS)) {
    Serial.println("STA Failed to configure Static IP");
  }

  // 2. Configure Custom AP IP Address (192.168.10.1)
  IPAddress customAPIP(192, 168, 10, 1);
  IPAddress customGateway(192, 168, 10, 1);
  IPAddress customSubnet(255, 255, 255, 0);
  WiFi.softAPConfig(customAPIP, customGateway, customSubnet);
  
  // 3. Start Access Point (AP) Network
  WiFi.softAP(ap_ssid, ap_password);
  Serial.print("Access Point Started! Connect to Wi-Fi: ");
  Serial.println(ap_ssid);
  Serial.print("AP IP Address: ");
  Serial.println(WiFi.softAPIP()); // Output: 192.168.10.1

  // 4. Connect to Home Router
  WiFi.begin(ssid, password);
  Serial.print("Connecting to Home Wi-Fi");
  int wifiAttempts = 0;
  while (WiFi.status() != WL_CONNECTED && wifiAttempts < 20) {
    delay(500);
    Serial.print(".");
    wifiAttempts++;
  }
  Serial.println();
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Connected to Home Router! IP Address: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("Failed to connect to home router, running in Direct AP mode.");
  }

  // --- Initialize OTA ---
  ArduinoOTA.setHostname("GrowBoxController");
  ArduinoOTA.onStart([]() { Serial.println("OTA Update Starting..."); });
  ArduinoOTA.onEnd([]() { Serial.println("\nOTA Update Complete!"); });
  ArduinoOTA.begin();

  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/light", HTTP_GET, handleLightControl);
  server.on("/fan", HTTP_GET, handleFanControl);
  
  server.begin();
  Serial.println("Web server and OTA started!");

  checkOpenWeatherMap();
  lastWeatherCheck = millis();
}

void loop() {
  server.handleClient();
  ArduinoOTA.handle();

  unsigned long currentMillis = millis();

  // --- Periodic Weather Check (Every 15 mins) ---
  if (currentMillis - lastWeatherCheck >= WEATHER_CHECK_INTERVAL) {
    lastWeatherCheck = currentMillis;
    checkOpenWeatherMap();
  }

  // Get current NTP time structure
  time_t now;
  struct tm* timeinfo;
  time(&now);
  timeinfo = localtime(&now);
  bool timeSynced = (timeinfo->tm_year > (2020 - 1900));

  // --- 1. GROW LIGHT LOGIC (Strict Manual Override First) ---
  bool lightShouldBeOn = false;
  
  if (lightManualMode == 1) {
    lightShouldBeOn = true;  // Forced ON
    secondsUntilLightToggle = 0;
  } else if (lightManualMode == 2) {
    lightShouldBeOn = false; // Forced OFF
    secondsUntilLightToggle = 0;
  } else {
    // AUTO MODE: Schedule
    if (timeSynced) {
      int currentHour = timeinfo->tm_hour;
      
      bool withinLightTimeRange = false;
      if (config.lightStartHour <= config.lightEndHour) {
        withinLightTimeRange = (currentHour >= config.lightStartHour && currentHour < config.lightEndHour);
      } else {
        withinLightTimeRange = (currentHour >= config.lightStartHour || currentHour < config.lightEndHour);
      }

      if (withinLightTimeRange) {
        unsigned long lightOnDurationMillis = (unsigned long)config.lightOnMinutes * 60UL * 1000UL;
        unsigned long lightOffDurationMillis = (unsigned long)config.lightOffMinutes * 60UL * 1000UL;
        unsigned long currentLightInterval = lightStateActive ? lightOnDurationMillis : lightOffDurationMillis;

        if (lightPreviousMillis == 0) {
          lightPreviousMillis = currentMillis;
          lightStateActive = true;
        }

        if (currentMillis - lightPreviousMillis >= currentLightInterval) {
          lightPreviousMillis = currentMillis;
          lightStateActive = !lightStateActive;
          currentLightInterval = lightStateActive ? lightOnDurationMillis : lightOffDurationMillis;
        }

        unsigned long elapsedInLightPhase = currentMillis - lightPreviousMillis;
        unsigned long remainingInLightPhase = (currentLightInterval > elapsedInLightPhase) ? (currentLightInterval - elapsedInLightPhase) : 0;
        
        secondsUntilLightToggle = remainingInLightPhase / 1000UL;
        lightShouldBeOn = lightStateActive;
      } else {
        lightStateActive = false;
        lightShouldBeOn = false; 
        secondsUntilLightToggle = 0;
        lightPreviousMillis = 0;
      }
    } else {
      lightShouldBeOn = false;
      secondsUntilLightToggle = 0;
    }
  }
  
  // Explicit Light Relay Output (Change HIGH/LOW inside if/else if needed)
  if (lightShouldBeOn) {
    digitalWrite(lightRelayPin, HIGH); 
  } else {
    digitalWrite(lightRelayPin, LOW);  
  }

  // --- 2. FAN LOGIC (Strict Manual Override First) ---
  bool fanShouldBeOn = false;
  
  if (fanManualMode == 1) {
    fanShouldBeOn = true;  // Forced ON
    secondsUntilFanOn = 0;
  } else if (fanManualMode == 2 || isWindyOverride) {
    fanShouldBeOn = false; // Forced OFF (Manual or Windy Weather Override)
    secondsUntilFanOn = 0; 
  } else {
    // AUTO MODE: Schedule
    if (timeSynced) {
      int currentHour = timeinfo->tm_hour;
      
      bool withinTimeRange = false;
      if (config.fanStartHour <= config.fanEndHour) {
        withinTimeRange = (currentHour >= config.fanStartHour && currentHour < config.fanEndHour);
      } else {
        withinTimeRange = (currentHour >= config.fanStartHour || currentHour < config.fanEndHour);
      }

      if (withinTimeRange) {
        unsigned long fanOnDurationMillis = (unsigned long)config.fanOnSeconds * 1000UL;
        unsigned long fanOffDurationMillis = (unsigned long)config.fanOffMinutes * 60UL * 1000UL;
        unsigned long currentInterval = fanStateActive ? fanOnDurationMillis : fanOffDurationMillis;

        if (fanPreviousMillis == 0) {
          fanPreviousMillis = currentMillis;
          fanStateActive = true;
        }

        if (currentMillis - fanPreviousMillis >= currentInterval) {
          fanPreviousMillis = currentMillis;
          fanStateActive = !fanStateActive;
          currentInterval = fanStateActive ? fanOnDurationMillis : fanOffDurationMillis;
        }

        unsigned long elapsedInPhase = currentMillis - fanPreviousMillis;
        unsigned long remainingInPhase = (currentInterval > elapsedInPhase) ? (currentInterval - elapsedInPhase) : 0;

        if (fanStateActive) {
          secondsUntilFanOn = (remainingInPhase + fanOffDurationMillis) / 1000UL;
        } else {
          secondsUntilFanOn = remainingInPhase / 1000UL;
        }

        fanShouldBeOn = fanStateActive;
      } else {
        fanStateActive = false;
        fanShouldBeOn = false; 
        secondsUntilFanOn = 0; 
        fanPreviousMillis = 0; 
      }
    } else {
      fanShouldBeOn = false;
      secondsUntilFanOn = 0;
    }
  }

  // Explicit Fan Relay Output (Change HIGH/LOW inside if/else if needed)
  if (fanShouldBeOn) {
    digitalWrite(fanRelayPin, HIGH); 
  } else {
    digitalWrite(fanRelayPin, LOW);  
  }
}