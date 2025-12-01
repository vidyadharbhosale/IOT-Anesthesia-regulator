#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <Wire.h>
#include <Servo.h>
#include "MAX30100_PulseOximeter.h"
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ======= USER CONFIG =======
const char* SSID = "YOUR_SSID";
const char* PASSWORD = "YOUR_PASSWORD";

const uint16_t REPORTING_PERIOD_MS = 1000; // OLED / Serial / web data update interval

// Thresholds (tweak as required)
const float BPM_THRESHOLD = 100.0;   // example threshold for heart rate
const float TEMP_THRESHOLD = 38.0;   // example threshold in °C

// Pins
const int SERVO_PIN = D4; // GPIO2 - servo signal
// LM35 -> A0 (ADC)
// MAX30100 uses I2C (SDA D2, SCL D1)
const int LM35_PIN = A0;

// ======= HARDWARE OBJECTS =======
ESP8266WebServer server(80);
PulseOximeter pox;
Adafruit_SSD1306 display(128, 64, &Wire, -1);
Servo myServo;

// ======= GLOBAL STATE =======
volatile float currentBPM = 0.0;
volatile float currentSpO2 = 0.0;
volatile float currentTemp = 0.0;

bool hasTurned = false;      // ensure servo turns only once
bool waiting = false;        // waiting during 10s non-blocking delay
unsigned long waitStart = 0;
const unsigned long WAIT_DURATION = 10000UL; // 10 seconds

unsigned long tsLastReport = 0;

// ======= HTML page (ptr) - minimal JS polls /data every second =======
String ptr = "";
ptr += "<!DOCTYPE html>";
ptr += "<html>";
ptr += "<head>";
ptr += "  <meta name='viewport' content='width=device-width, initial-scale=1'>";
ptr += "  <title>Patient Health Monitor</title>";
ptr += "  <link rel='stylesheet' href='https://cdnjs.cloudflare.com/ajax/libs/font-awesome/5.7.2/css/all.min.css'>";
ptr += "  <style>";
ptr += "    body{font-family:Arial,Helvetica,sans-serif;background:#f5f8fa;margin:0;padding:0;display:flex;justify-content:center;align-items:center;height:100vh}";
ptr += "    .card{background:#fff;padding:18px;border-radius:10px;box-shadow:0 6px 18px rgba(0,0,0,0.08);width:320px}";
ptr += "    h1{font-size:20px;color:#2c3e50;margin:0 0 12px 0;text-align:center}";
ptr += "    .row{display:flex;justify-content:space-between;padding:8px 0;border-bottom:1px solid #eee}";
ptr += "    .label{color:#666}";
ptr += "    .value{font-weight:700}";
ptr += "    .heart{color:#ff3b30}";
ptr += "    .spo2{color:#007bff}";
ptr += "    .temp{color:#ff9900}";
ptr += "  </style>";
ptr += "  <script>";
ptr += "    function fetchData(){";
ptr += "      fetch('/data').then(r=>r.json()).then(d=>{";
ptr += "        document.getElementById('bpm').innerText = d.bpm;";
ptr += "        document.getElementById('spo2').innerText = d.spo2;";
ptr += "        document.getElementById('temp').innerText = d.temp;";
ptr += "      }).catch(e=>console.log('err',e));";
ptr += "    }";
ptr += "    setInterval(fetchData,1000);";
ptr += "    window.onload = fetchData;";
ptr += "  </script>";
ptr += "</head>";
ptr += "<body>";
ptr += "  <div class='card'>";
ptr += "    <h1>Patient Health Monitor</h1>";
ptr += "    <div class='row'><div class='label'>Heart Rate</div><div class='value heart' id='bpm'>--</div></div>";
ptr += "    <div class='row'><div class='label'>SpO2</div><div class='value spo2' id='spo2'>--</div></div>";
ptr += "    <div class='row'><div class='label'>Temperature (°C)</div><div class='value temp' id='temp'>--</div></div>";
ptr += "  </div>";
ptr += "</body>";
ptr += "</html>";

// ======= Callbacks =======
void onBeatDetected() {
  // optional: you can flash an LED here or log
}

// ======= Helper functions =======
float readLM35Celsius() {
  // Convert ADC reading to voltage and then to Celsius for LM35 (10mV/°C).
  // On NodeMCU ADC range is 0-1.0V mapped to 0-1023 (if using default core),
  // but many NodeMCU dev boards provide ADC scaled to 0-3.3V.
  // We will assume ADC range is 0 - 3.3V here. If your board differs, change VREF.
  const float VREF = 3.3; // adjust if your ADC reference differs
  int adc = analogRead(LM35_PIN);
  float voltage = adc * (VREF / 1023.0);
  float tempC = voltage * 100.0; // LM35: 10mV per °C
  return tempC;
}

String jsonData() {
  String s = "{";
  s += "\"bpm\":"; s += String(currentBPM, 1); s += ",";
  s += "\"spo2\":"; s += String(currentSpO2, 1); s += ",";
  s += "\"temp\":"; s += String(currentTemp, 1);
  s += "}";
  return s;
}

// ======= Web handlers =======
void handleRoot() {
  server.send(200, "text/html", ptr);
}

void handleData() {
  String j = jsonData();
  server.send(200, "application/json", j);
}

void handleNotFound() {
  server.send(404, "text/plain", "Not found");
}

// ======= Setup =======
void setup() {
  Serial.begin(115200);
  delay(100);

  // Attach servo and set initial position
  myServo.attach(SERVO_PIN);
  myServo.write(0);

  // OLED init
  Wire.begin(D2, D1); // SDA = D2, SCL = D1
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("SSD1306 allocation failed");
    // continue without OLED
  } else {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(WHITE);
    display.setCursor(0, 0);
    display.println("Patient Health Monitor");
    display.display();
  }

  // Initialize PulseOximeter
  if (!pox.begin()) {
    Serial.println("Failed to initialize MAX30100");
    // Continue — but pox.update() will do nothing
  } else {
    pox.setOnBeatDetectedCallback(onBeatDetected);
    pox.setIRLedCurrent(MAX30100_LED_CURR_7_6MA);
    Serial.println("MAX30100 initialized");
  }

  // WiFi
  WiFi.mode(WIFI_STA);
  WiFi.begin(SSID, PASSWORD);
  Serial.print("Connecting to WiFi ");
  Serial.print(SSID);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000UL) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Connected! IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("WiFi connect failed - start AP mode");
    WiFi.softAP("Anesthesia_Mon_AP");
    Serial.print("AP IP: ");
    Serial.println(WiFi.softAPIP());
  }

  // Web handlers
  server.on("/", HTTP_GET, handleRoot);
  server.on("/data", HTTP_GET, handleData);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("HTTP server started");

  tsLastReport = millis();
}

// ======= Main loop =======
void loop() {
  // Keep webserver responsive
  server.handleClient();

  // Keep pulse oximeter updated as often as possible
  pox.update();

  // Read sensors and update globals
  unsigned long now = millis();
  if (now - tsLastReport >= REPORTING_PERIOD_MS) {
    // BPM & SpO2 from MAX30100
    currentBPM = pox.getHeartRate();
    currentSpO2 = pox.getSpO2();

    // Temperature from LM35
    currentTemp = readLM35Celsius();

    // Debug
    Serial.print("BPM: "); Serial.print(currentBPM);
    Serial.print("  SpO2: "); Serial.print(currentSpO2);
    Serial.print("  Temp: "); Serial.println(currentTemp);

    // Update OLED if present
    if (display.width() > 0) {
      display.clearDisplay();
      display.setCursor(0, 0);
      display.setTextSize(1);
      display.println("Patient Health Monitor");
      display.println();
      display.setTextSize(2);
      display.print("HR: "); display.print(currentBPM, 0); display.println(" bpm");
      display.print("SpO2: "); display.print(currentSpO2, 0); display.println("%");
      display.print("Temp: "); display.print(currentTemp, 1); display.println(" C");
      display.display();
    }

    tsLastReport = now;
  }

  // Threshold logic & non-blocking 10s wait
  // Trigger if sensor values exceed thresholds and servo hasn't moved
  if (!hasTurned && !waiting) {
    // Use any condition you want; here we trigger when BPM > BPM_THRESHOLD OR Temp > TEMP_THRESHOLD
    if (currentBPM > BPM_THRESHOLD || currentTemp > TEMP_THRESHOLD) {
      waiting = true;
      waitStart = millis();
      Serial.println("Threshold exceeded — starting 10s wait (non-blocking)...");
    }
  }

  // If waiting, check elapsed time; continue updating pox during this time
  if (waiting && !hasTurned) {
    if (millis() - waitStart >= WAIT_DURATION) {
      // After 10s elapse, move servo once
      // Optional: verify values still exceed thresholds before moving
      if (currentBPM > BPM_THRESHOLD || currentTemp > TEMP_THRESHOLD) {
        Serial.println("10s elapsed and threshold still exceeded — moving servo to 180°");
        myServo.write(180);
        hasTurned = true;
      } else {
        Serial.println("10s elapsed but threshold no longer exceeded — canceling");
      }
      waiting = false;
    }
  }

  // small yield to allow wifi stack and background tasks
  delay(20);
}// Full source code provided previously.
