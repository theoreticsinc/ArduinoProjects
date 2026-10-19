#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>

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

ESP8266WebServer server(80);

// --- Timer Durations (in seconds) ---
unsigned long durationON = 1 * 60;     // 1 Minute ON
unsigned long durationOFF = 2 * 60;    // 2 Minutes OFF
unsigned long durationWAIT = 15 * 60;  // 15 Minutes WAIT

// --- State Machine ---
enum CycleState { MANUAL,
                  STATE_ON,
                  STATE_OFF,
                  STATE_WAIT };
//CycleState currentState = MANUAL;
CycleState currentState = STATE_ON;

unsigned long previousMillis = 0;
//bool pumpState = false;
bool pumpState = true;

// Function to get remaining seconds for the current active state
long getRemainingSeconds() {
  if (currentState == MANUAL) return 0;

  unsigned long currentMillis = millis();
  unsigned long elapsedSec = (currentMillis - previousMillis) / 1000;
  unsigned long targetSec = 0;

  if (currentState == STATE_ON) targetSec = durationON;
  if (currentState == STATE_OFF) targetSec = durationOFF;
  if (currentState == STATE_WAIT) targetSec = durationWAIT;

  if (elapsedSec >= targetSec) return 0;
  return targetSec - elapsedSec;
}

void handleRoot() {
  long remainingSec = getRemainingSeconds();

  String html = "<!DOCTYPE html><html><head><title>Hydroponics 3-Step Control</title>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";

  // Auto-refresh the page every 3 seconds to keep UI synced with ESP8266
  html += "<meta http-equiv='refresh' content='3'>";

  html += "<style>body{font-family:Arial;text-align:center;margin-top:20px;background:#f4f4f9;}";
  html += ".card{background:white;padding:20px;margin:10px auto;max-width:400px;border-radius:10px;box-shadow:0 2px 5px rgba(0,0,0,0.2);}";
  html += ".btn{padding:10px 20px;font-size:16px;margin:5px;border:none;border-radius:5px;cursor:pointer;}";
  html += ".on{background-color:#4CAF50;color:white;} .off{background-color:#f44336;color:white;}";
  html += ".timer-box{font-size:32px;font-weight:bold;color:#2196F3;margin:15px 0;}";
  html += "input[type=number]{padding:6px;width:70px;margin:5px;}</style></head><body>";

  html += "<div class='card'>";
  html += "<h2>Hydroponics Controller</h2>";

  String stateStr = "MANUAL MODE";
  if (currentState == STATE_ON) stateStr = "STEP 1: PUMP ON";
  if (currentState == STATE_OFF) stateStr = "STEP 2: PUMP OFF";
  if (currentState == STATE_WAIT) stateStr = "STEP 3: WAITING";

  html += "<h3>Status: <span style='color:" + String(pumpState ? "green" : "red") + "'>" + String(pumpState ? "PUMP RUNNING" : "PUMP STOPPED") + "</span></h3>";
  html += "<p>Mode: <b>" + stateStr + "</b></p>";

  // Display Countdown
  if (currentState != MANUAL) {
    html += "<p>Time Remaining in Current Step:</p>";
    html += "<div class='timer-box' id='countdown'>" + String(remainingSec / 60) + "m " + String(remainingSec % 60) + "s</div>";
  } else {
    html += "<div class='timer-box'>--:--</div>";
  }

  // Manual Control Buttons
  html += "<h3>Manual Control</h3>";
  html += "<a href='/manual?state=on'><button class='btn on'>Turn ON</button></a>";
  html += "<a href='/manual?state=off'><button class='btn off'>Turn OFF</button></a>";
  html += "</div>";

  // Cycle Settings Card
  html += "<div class='card'>";
  html += "<h3>3-Step Cycle Setup</h3>";
  html += "<form action='/setTimer' method='GET'>";
  html += "Step 1 - ON (min): <input type='number' name='tOn' value='" + String(durationON / 60) + "' min='1'><br>";
  html += "Step 2 - OFF (min): <input type='number' name='tOff' value='" + String(durationOFF / 60) + "' min='1'><br>";
  html += "Step 3 - WAIT (min): <input type='number' name='tWait' value='" + String(durationWAIT / 60) + "' min='1'><br><br>";
  html += "<input type='submit' name='mode' value='Start Cycle' class='btn on'>";
  html += "<input type='submit' name='mode' value='Stop Cycle' class='btn off'>";
  html += "</form>";
  html += "</div>";

  // Client-side JavaScript Countdown Script
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
  // Proper HTTP 303 Redirect back to Root
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleSetTimer() {
  if (server.hasArg("tOn")) durationON = server.arg("tOn").toInt() * 60;
  if (server.hasArg("tOff")) durationOFF = server.arg("tOff").toInt() * 60;
  if (server.hasArg("tWait")) durationWAIT = server.arg("tWait").toInt() * 60;

  if (server.hasArg("mode")) {
    String mode = server.arg("mode");
    if (mode == "Start Cycle") {
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
  // Proper HTTP 303 Redirect back to Root
  server.sendHeader("Location", "/");
  server.send(303);
}

void setup() {
  Serial.begin(115200);
  pinMode(RELAY_PIN, OUTPUT);
  //digitalWrite(RELAY_PIN, PUMP_OFF);
  // Inside setup(): Set initial relay output to PUMP_ON and initialize timer
  digitalWrite(RELAY_PIN, PUMP_ON);
  previousMillis = millis();

  WiFi.mode(WIFI_STA);
  WiFi.config(local_IP, gateway, subnet, primaryDNS);
  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
  }

  server.on("/", handleRoot);
  server.on("/manual", handleManual);
  server.on("/setTimer", handleSetTimer);
  server.begin();
}

void loop() {
  server.handleClient();

  // State Machine Timing logic
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