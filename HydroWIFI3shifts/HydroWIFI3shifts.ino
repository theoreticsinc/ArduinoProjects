#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <time.h>

// --- WiFi Credentials ---
const char* ssid = "PLDTHOMEFIBR2XamC_2.4G";
const char* password = "PLDTWIFI6UnMM";

// --- Static IP Configuration ---
IPAddress local_IP(192, 168, 1, 120);  // Static IP you want to set
IPAddress gateway(192, 168, 1, 1);     // Your router's IP address (usually 192.168.1.1 or 192.168.1.254)
IPAddress subnet(255, 255, 255, 0);    // Subnet mask
IPAddress primaryDNS(8, 8, 8, 8);      // Optional: Google DNS

// --- Pin Setup ---
const int RELAY_PIN = D5;  // Active LOW Relay: ON = LOW, OFF = HIGH
#define PUMP_ON LOW
#define PUMP_OFF HIGH
// --- Time Zone / NTP Settings ---
#define MY_TZ "PHT-8" // Update timezone string if needed
const char* ntpServer = "pool.ntp.org";

ESP8266WebServer server(80);

// --- 3 Shift Definitions (Minutes) ---
struct ShiftConfig {
  int startHour;            // Start hour in 24h format (0 - 23)
  unsigned long durationON;  // ON duration in minutes
  unsigned long durationOFF; // OFF duration in minutes
  unsigned long durationWAIT;// WAIT duration in minutes
  String name;
};

// Default Shifts: Morning (6 AM), Day (12 PM), Night (6 PM)
ShiftConfig shift1 = { 6, 1, 2, 15, "Shift 1 (Morning)" };
ShiftConfig shift2 = {12, 1, 2, 20, "Shift 2 (Day)"     };
ShiftConfig shift3 = {18, 1, 2, 30, "Shift 3 (Night)"   };

// Active Cycle Durations (in seconds)
unsigned long durationON   = 60;
unsigned long durationOFF  = 120;
unsigned long durationWAIT = 900;
String activeShiftName = "Shift 1";

// --- State Machine ---
enum CycleState { MANUAL, STATE_ON, STATE_OFF, STATE_WAIT };
CycleState currentState = STATE_ON;

unsigned long previousMillis = 0;
bool pumpState = true;

// Determine current shift based on current hour
void updateShiftProfile() {
  time_t now = time(nullptr);
  struct tm* timeinfo = localtime(&now);

  if (timeinfo->tm_year > 70) {
    int currentHour = timeinfo->tm_hour;

    // Evaluate which shift window the current hour falls into
    if ((shift1.startHour < shift2.startHour && currentHour >= shift1.startHour && currentHour < shift2.startHour) ||
        (shift1.startHour > shift2.startHour && (currentHour >= shift1.startHour || currentHour < shift2.startHour))) {
      activeShiftName = shift1.name;
      durationON   = shift1.durationON * 60;
      durationOFF  = shift1.durationOFF * 60;
      durationWAIT = shift1.durationWAIT * 60;
    } 
    else if ((shift2.startHour < shift3.startHour && currentHour >= shift2.startHour && currentHour < shift3.startHour) ||
             (shift2.startHour > shift3.startHour && (currentHour >= shift2.startHour || currentHour < shift3.startHour))) {
      activeShiftName = shift2.name;
      durationON   = shift2.durationON * 60;
      durationOFF  = shift2.durationOFF * 60;
      durationWAIT = shift2.durationWAIT * 60;
    } 
    else {
      activeShiftName = shift3.name;
      durationON   = shift3.durationON * 60;
      durationOFF  = shift3.durationOFF * 60;
      durationWAIT = shift3.durationWAIT * 60;
    }
  }
}

// Calculate remaining seconds for state countdown
long getRemainingSeconds() {
  if (currentState == MANUAL) return 0;
  
  unsigned long currentMillis = millis();
  unsigned long elapsedSec = (currentMillis - previousMillis) / 1000;
  unsigned long targetSec = 0;

  if (currentState == STATE_ON)   targetSec = durationON;
  if (currentState == STATE_OFF)  targetSec = durationOFF;
  if (currentState == STATE_WAIT) targetSec = durationWAIT;

  if (elapsedSec >= targetSec) return 0;
  return targetSec - elapsedSec;
}

void handleRoot() {
  updateShiftProfile();
  long remainingSec = getRemainingSeconds();

  time_t now = time(nullptr);
  struct tm* timeinfo = localtime(&now);
  char timeStr[30] = "Syncing Time...";
  if (timeinfo->tm_year > 70) {
    strftime(timeStr, sizeof(timeStr), "%I:%M:%S %p", timeinfo);
  }

  String html = "<!DOCTYPE html><html><head><title>Hydroponics 3-Shift Control</title>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<meta http-equiv='refresh' content='3'>"; 

  html += "<style>body{font-family:Arial;text-align:center;margin-top:20px;background:#f4f4f9;}";
  html += ".card{background:white;padding:20px;margin:10px auto;max-width:460px;border-radius:10px;box-shadow:0 2px 5px rgba(0,0,0,0.2);}";
  html += ".btn{padding:10px 20px;font-size:16px;margin:5px;border:none;border-radius:5px;cursor:pointer;}";
  html += ".on{background-color:#4CAF50;color:white;} .off{background-color:#f44336;color:white;}";
  html += ".timer-box{font-size:32px;font-weight:bold;color:#2196F3;margin:10px 0;}";
  html += ".badge{padding:5px 10px;border-radius:5px;color:white;font-weight:bold;background-color:#009688;}";
  html += "input[type=number]{padding:5px;width:50px;margin:2px;}</style></head><body>";

  html += "<div class='card'>";
  html += "<h2>Hydroponics 3-Shift Controller</h2>";
  html += "<p>Current Time: <b>" + String(timeStr) + "</b></p>";
  html += "<p>Active Profile: <span class='badge'>" + activeShiftName + "</span></p>";

  String stateStr = "MANUAL MODE";
  if (currentState == STATE_ON)   stateStr = "STEP 1: PUMP ON";
  if (currentState == STATE_OFF)  stateStr = "STEP 2: PUMP OFF";
  if (currentState == STATE_WAIT) stateStr = "STEP 3: WAITING";

  html += "<h3>Status: <span style='color:" + String(pumpState ? "green" : "red") + "'>" + String(pumpState ? "RUNNING" : "STOPPED") + "</span></h3>";
  html += "<p>Mode: <b>" + stateStr + "</b></p>";

  // Display Countdown
  if (currentState != MANUAL) {
    html += "<p>Time Remaining in Current Step:</p>";
    html += "<div class='timer-box' id='countdown'>" + String(remainingSec / 60) + "m " + String(remainingSec % 60) + "s</div>";
  } else {
    html += "<div class='timer-box'>--:--</div>";
  }

  // Manual Override
  html += "<h3>Manual Control</h3>";
  html += "<a href='/manual?state=on'><button class='btn on'>Turn ON</button></a>";
  html += "<a href='/manual?state=off'><button class='btn off'>Turn OFF</button></a>";
  html += "</div>";

  // Form for Shift Setup
  html += "<div class='card'>";
  html += "<h3>Configure 3 Shifts</h3>";
  html += "<form action='/setTimer' method='GET'>";
  
  // Shift 1 Config
  html += "<b>Shift 1 Start Hour (0-23):</b> <input type='number' name='s1H' value='" + String(shift1.startHour) + "' min='0' max='23'><br>";
  html += "ON: <input type='number' name='s1On' value='" + String(shift1.durationON) + "'>m ";
  html += "OFF: <input type='number' name='s1Off' value='" + String(shift1.durationOFF) + "'>m ";
  html += "WAIT: <input type='number' name='s1Wait' value='" + String(shift1.durationWAIT) + "'>m<br><br>";

  // Shift 2 Config
  html += "<b>Shift 2 Start Hour (0-23):</b> <input type='number' name='s2H' value='" + String(shift2.startHour) + "' min='0' max='23'><br>";
  html += "ON: <input type='number' name='s2On' value='" + String(shift2.durationON) + "'>m ";
  html += "OFF: <input type='number' name='s2Off' value='" + String(shift2.durationOFF) + "'>m ";
  html += "WAIT: <input type='number' name='s2Wait' value='" + String(shift2.durationWAIT) + "'>m<br><br>";

  // Shift 3 Config
  html += "<b>Shift 3 Start Hour (0-23):</b> <input type='number' name='s3H' value='" + String(shift3.startHour) + "' min='0' max='23'><br>";
  html += "ON: <input type='number' name='s3On' value='" + String(shift3.durationON) + "'>m ";
  html += "OFF: <input type='number' name='s3Off' value='" + String(shift3.durationOFF) + "'>m ";
  html += "WAIT: <input type='number' name='s3Wait' value='" + String(shift3.durationWAIT) + "'>m<br><br>";

  html += "<input type='submit' name='mode' value='Save & Start' class='btn on'>";
  html += "<input type='submit' name='mode' value='Stop Auto' class='btn off'>";
  html += "</form>";
  html += "</div>";

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
  html += "</script>";

  html += "</body></html>";
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
  // Update Shift 1
  if (server.hasArg("s1H"))    shift1.startHour    = server.arg("s1H").toInt();
  if (server.hasArg("s1On"))   shift1.durationON   = server.arg("s1On").toInt();
  if (server.hasArg("s1Off"))  shift1.durationOFF  = server.arg("s1Off").toInt();
  if (server.hasArg("s1Wait")) shift1.durationWAIT = server.arg("s1Wait").toInt();

  // Update Shift 2
  if (server.hasArg("s2H"))    shift2.startHour    = server.arg("s2H").toInt();
  if (server.hasArg("s2On"))   shift2.durationON   = server.arg("s2On").toInt();
  if (server.hasArg("s2Off"))  shift2.durationOFF  = server.arg("s2Off").toInt();
  if (server.hasArg("s2Wait")) shift2.durationWAIT = server.arg("s2Wait").toInt();

  // Update Shift 3
  if (server.hasArg("s3H"))    shift3.startHour    = server.arg("s3H").toInt();
  if (server.hasArg("s3On"))   shift3.durationON   = server.arg("s3On").toInt();
  if (server.hasArg("s3Off"))  shift3.durationOFF  = server.arg("s3Off").toInt();
  if (server.hasArg("s3Wait")) shift3.durationWAIT = server.arg("s3Wait").toInt();

  if (server.hasArg("mode")) {
    String mode = server.arg("mode");
    if (mode == "Save & Start") {
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

  WiFi.mode(WIFI_STA);
  WiFi.config(local_IP, gateway, subnet, primaryDNS);
  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
  }

  configTime(MY_TZ, ntpServer);

  server.on("/", handleRoot);
  server.on("/manual", handleManual);
  server.on("/setTimer", handleSetTimer);
  server.begin();
}

void loop() {
  server.handleClient();

  if (currentState != MANUAL) {
    unsigned long currentMillis = millis();

    switch (currentState) {
      case STATE_ON:
        if (currentMillis - previousMillis >= (durationON * 1000)) {
          currentState = STATE_OFF;
          digitalWrite(RELAY_PIN, PUMP_OFF);
          pumpState = false;
          previousMillis = currentMillis;
        }
        break;

      case STATE_OFF:
        if (currentMillis - previousMillis >= (durationOFF * 1000)) {
          currentState = STATE_WAIT;
          digitalWrite(RELAY_PIN, PUMP_OFF);
          pumpState = false;
          previousMillis = currentMillis;
        }
        break;

      case STATE_WAIT:
        if (currentMillis - previousMillis >= (durationWAIT * 1000)) {
          // Re-evaluate current hour to pick the active shift rules
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