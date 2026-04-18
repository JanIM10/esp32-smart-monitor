/**************************************************************
 * ESP32 Smart Environmental Monitoring System
 * ------------------------------------------------------------
 * Features:
 * - Reads temperature and humidity using a DHT11 sensor
 * - Reads ambient light level using a photoresistor on an ADC pin
 * - Triggers LED and buzzer alerts based on configurable thresholds
 * - Hosts a local Wi-Fi dashboard over HTTP
 * - Saves threshold settings in ESP32 non-volatile memory
 * - Displays local trend graphs using the most recent sensor history
 **************************************************************/

#include <WiFi.h>
#include <Preferences.h>
#include "DHT.h"

/******************** PIN DEFINITIONS *************************/
// Sensor and output pin assignments
#define DHTPIN 4
#define DHTTYPE DHT11
#define LIGHTPIN 34
#define LEDPIN 2
#define BUZZERPIN 18

/******************** WIFI CONFIGURATION **********************/
// Replace these with your actual Wi-Fi credentials before use
const char* ssid = "YOUR_WIFI_NAME";
const char* password = "YOUR_PASSWORD";

/******************** GLOBAL OBJECTS **************************/
// HTTP server runs on port 80
WiFiServer server(80);

// Sensor and preference-storage objects
DHT dht(DHTPIN, DHTTYPE);
Preferences prefs;

/******************** THRESHOLD SETTINGS **********************/
// Default thresholds used for alert logic
float tempThreshold = 30.0;
int lightThreshold = 1000;

/******************** LIVE SENSOR STATE ***********************/
// Most recent processed sensor readings
float temperature = 0.0;
float humidity = 0.0;
int lightValue = 0;

// True when temperature or light crosses configured threshold
bool alertState = false;

/******************** TIMING CONTROL **************************/
// Used to read sensors periodically without blocking the server loop
unsigned long lastSensorRead = 0;
const unsigned long sensorInterval = 2000;  // milliseconds

/******************** HISTORY STORAGE *************************/
// Used to build local dashboard graphs from recent readings
const int HISTORY_SIZE = 20;
float tempHistory[HISTORY_SIZE];
int lightHistory[HISTORY_SIZE];

/******************** STATUS HELPER FUNCTIONS *****************/

// Returns a human-readable explanation for the current alert state
String getAlertReason() {
  bool tempHigh = temperature > tempThreshold;
  bool lightLow = lightValue < lightThreshold;

  if (tempHigh && lightLow) return "High temperature and low light";
  if (tempHigh) return "High temperature";
  if (lightLow) return "Low light";
  return "System normal";
}

// Converts raw light reading into a simple status label
String getLightLabel() {
  if (lightValue < 800) return "Dark";
  if (lightValue < 2500) return "Normal";
  return "Bright";
}

// Converts temperature into a simple status label
String getTempLabel() {
  if (temperature < 20.0) return "Cool";
  if (temperature < tempThreshold) return "Normal";
  return "High";
}

// Returns formatted uptime string for dashboard display
String uptimeString() {
  unsigned long sec = millis() / 1000;
  unsigned long hrs = sec / 3600;
  unsigned long mins = (sec % 3600) / 60;
  unsigned long secs = sec % 60;
  return String(hrs) + "h " + String(mins) + "m " + String(secs) + "s";
}

/******************** HISTORY MANAGEMENT **********************/

// Initializes history arrays so the graph has known starting values
void initializeHistory() {
  for (int i = 0; i < HISTORY_SIZE; i++) {
    tempHistory[i] = 0.0;
    lightHistory[i] = 0;
  }
}

// Shifts older readings left and inserts the newest value at the end
void updateHistory(float newTemp, int newLight) {
  for (int i = 0; i < HISTORY_SIZE - 1; i++) {
    tempHistory[i] = tempHistory[i + 1];
    lightHistory[i] = lightHistory[i + 1];
  }
  tempHistory[HISTORY_SIZE - 1] = newTemp;
  lightHistory[HISTORY_SIZE - 1] = newLight;
}

/******************** NON-VOLATILE STORAGE ********************/

// Loads saved thresholds from ESP32 flash memory
// If no saved values exist yet, defaults are used instead
void loadSavedSettings() {
  prefs.begin("monitor", true);
  tempThreshold = prefs.getFloat("tempTh", 30.0);
  lightThreshold = prefs.getInt("lightTh", 1000);
  prefs.end();

  Serial.println("Loaded saved settings:");
  Serial.print("Temp Threshold: ");
  Serial.println(tempThreshold);
  Serial.print("Light Threshold: ");
  Serial.println(lightThreshold);
}

// Saves current threshold values to ESP32 flash memory
void saveSettings() {
  prefs.begin("monitor", false);
  prefs.putFloat("tempTh", tempThreshold);
  prefs.putInt("lightTh", lightThreshold);
  prefs.end();

  Serial.println("Settings saved.");
}

/******************** SENSOR ACQUISITION **********************/

// Reads sensors at a fixed interval, updates outputs, and stores history
void readSensors() {
  // Only read again if enough time has passed since last sample
  if (millis() - lastSensorRead >= sensorInterval) {
    lastSensorRead = millis();

    float newHumidity = dht.readHumidity();
    float newTemperature = dht.readTemperature();
    int newLightValue = analogRead(LIGHTPIN);

    // Only update DHT values if they are valid
    if (!isnan(newHumidity) && !isnan(newTemperature)) {
      humidity = newHumidity;
      temperature = newTemperature;
    }

    // Light reading is always updated from ADC
    lightValue = newLightValue;

    // Alert is active if either threshold condition is violated
    alertState = (temperature > tempThreshold) || (lightValue < lightThreshold);

    // Drive physical outputs based on alert state
    digitalWrite(LEDPIN, alertState ? HIGH : LOW);
    digitalWrite(BUZZERPIN, alertState ? HIGH : LOW);

    // Save latest values for graph generation
    updateHistory(temperature, lightValue);

    // Serial log for debugging and live monitoring
    Serial.print("Temp: ");
    Serial.print(temperature, 1);
    Serial.print(" C | Humidity: ");
    Serial.print(humidity, 1);
    Serial.print(" % | Light: ");
    Serial.print(lightValue);
    Serial.print(" | Alert: ");
    Serial.println(alertState ? "ON" : "OFF");
  }
}

/******************** REQUEST PARSING *************************/

// Parses incoming GET request parameters and updates thresholds
// Example request: /?temp=28.5&light=900
void handleThresholdUpdate(String request) {
  bool changed = false;

  int tempIndex = request.indexOf("temp=");
  int lightIndex = request.indexOf("light=");

  // Parse temperature threshold if present
  if (tempIndex != -1) {
    int tempEnd = request.indexOf('&', tempIndex);
    if (tempEnd == -1) tempEnd = request.indexOf(' ', tempIndex);
    String tempStr = request.substring(tempIndex + 5, tempEnd);
    float newTemp = tempStr.toFloat();

    // Accept only reasonable values
    if (newTemp >= 0.0 && newTemp <= 80.0 && newTemp != tempThreshold) {
      tempThreshold = newTemp;
      changed = true;
      Serial.print("Updated temp threshold: ");
      Serial.println(tempThreshold);
    }
  }

  // Parse light threshold if present
  if (lightIndex != -1) {
    int lightEnd = request.indexOf(' ', lightIndex);
    String lightStr = request.substring(lightIndex + 6, lightEnd);
    int newLight = lightStr.toInt();

    // Accept only valid ADC-range values
    if (newLight >= 0 && newLight <= 4095 && newLight != lightThreshold) {
      lightThreshold = newLight;
      changed = true;
      Serial.print("Updated light threshold: ");
      Serial.println(lightThreshold);
    }
  }

  // Save only if at least one value actually changed
  if (changed) {
    saveSettings();
  }
}

/******************** GRAPH HELPERS ***************************/

// Finds the maximum recorded temperature so bars can be scaled visually
float maxTempInHistory() {
  float maxVal = 1.0;
  for (int i = 0; i < HISTORY_SIZE; i++) {
    if (tempHistory[i] > maxVal) maxVal = tempHistory[i];
  }
  return maxVal;
}

// Builds HTML bars for the temperature trend graph
String buildTemperatureBars() {
  String bars = "";
  float maxVal = maxTempInHistory();

  for (int i = 0; i < HISTORY_SIZE; i++) {
    int height = (int)((tempHistory[i] / maxVal) * 100.0);

    // Prevent tiny valid bars from disappearing completely
    if (height < 4 && tempHistory[i] > 0.0) height = 4;
    if (tempHistory[i] <= 0.0) height = 2;

    bars += "<div class='bar-wrap'>";
    bars += "<div class='bar temp-bar' style='height:" + String(height) + "%;'></div>";
    bars += "</div>";
  }
  return bars;
}

// Builds HTML bars for the light trend graph
String buildLightBars() {
  String bars = "";

  for (int i = 0; i < HISTORY_SIZE; i++) {
    int height = (int)((lightHistory[i] / 4095.0) * 100.0);

    // Prevent tiny valid bars from disappearing completely
    if (height < 4 && lightHistory[i] > 0) height = 4;
    if (lightHistory[i] <= 0) height = 2;

    bars += "<div class='bar-wrap'>";
    bars += "<div class='bar light-bar' style='height:" + String(height) + "%;'></div>";
    bars += "</div>";
  }
  return bars;
}

/******************** DASHBOARD PAGE **************************/

// Builds the full HTML page served by the ESP32 web server
String buildDashboardPage() {
  String html = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>ESP32 Smart Monitor</title>
<style>
:root{
  --bg:#0b1220;
  --panel:#111a2e;
  --panel2:#16213b;
  --text:#edf2ff;
  --muted:#9fb0d3;
  --line:#243352;
  --blue:#60a5fa;
  --green:#34d399;
  --red:#f87171;
  --amber:#fbbf24;
  --shadow:0 14px 40px rgba(0,0,0,0.28);
}
*{box-sizing:border-box}
body{
  margin:0;
  font-family:Arial, Helvetica, sans-serif;
  background:linear-gradient(180deg,#08101d 0%, #0b1220 100%);
  color:var(--text);
}
.container{
  max-width:1150px;
  margin:0 auto;
  padding:20px 16px 36px;
}
.topbar{
  display:flex;
  justify-content:space-between;
  align-items:center;
  gap:14px;
  flex-wrap:wrap;
  margin-bottom:18px;
}
.brand{
  display:flex;
  flex-direction:column;
  gap:6px;
}
.title{
  font-size:30px;
  font-weight:700;
}
.subtitle{
  color:var(--muted);
  font-size:14px;
}
.chips{
  display:flex;
  gap:10px;
  flex-wrap:wrap;
}
.chip{
  background:rgba(255,255,255,0.05);
  border:1px solid var(--line);
  color:var(--muted);
  padding:10px 14px;
  border-radius:999px;
  font-size:13px;
}
.hero{
  background:linear-gradient(135deg,#101a31 0%, #16213b 100%);
  border:1px solid var(--line);
  border-radius:22px;
  padding:22px;
  box-shadow:var(--shadow);
  margin-bottom:18px;
}
.hero-row{
  display:flex;
  justify-content:space-between;
  align-items:flex-start;
  gap:18px;
  flex-wrap:wrap;
}
.hero-left h2{
  margin:0 0 8px 0;
  font-size:24px;
}
.hero-left p{
  margin:0;
  color:var(--muted);
  line-height:1.5;
}
.status-box{
  min-width:240px;
  background:rgba(255,255,255,0.04);
  border:1px solid var(--line);
  border-radius:18px;
  padding:18px;
}
.status-label{
  color:var(--muted);
  font-size:13px;
  margin-bottom:8px;
}
.status-main{
  font-size:28px;
  font-weight:700;
  margin-bottom:8px;
}
.status-on{color:var(--red)}
.status-off{color:var(--green)}
.status-reason{
  color:var(--muted);
  font-size:14px;
  line-height:1.4;
}
.main-grid{
  display:grid;
  grid-template-columns:2fr 1fr;
  gap:18px;
}
.left-col,.right-col{
  display:flex;
  flex-direction:column;
  gap:18px;
}
.section{
  background:var(--panel);
  border:1px solid var(--line);
  border-radius:20px;
  box-shadow:var(--shadow);
  overflow:hidden;
}
.section-header{
  padding:18px 20px;
  border-bottom:1px solid var(--line);
  background:rgba(255,255,255,0.02);
}
.section-title{
  margin:0;
  font-size:18px;
}
.section-sub{
  margin-top:6px;
  color:var(--muted);
  font-size:13px;
}
.cards{
  display:grid;
  grid-template-columns:repeat(3,1fr);
  gap:16px;
  padding:20px;
}
.card{
  background:var(--panel2);
  border:1px solid var(--line);
  border-radius:18px;
  padding:18px;
}
.card-label{
  color:var(--muted);
  font-size:13px;
  margin-bottom:12px;
}
.card-value{
  font-size:34px;
  font-weight:700;
  margin-bottom:10px;
}
.card-unit{
  font-size:16px;
  color:var(--muted);
  margin-left:4px;
}
.card-foot{
  display:flex;
  justify-content:space-between;
  align-items:center;
  gap:8px;
  flex-wrap:wrap;
}
.pill{
  display:inline-block;
  padding:7px 11px;
  border-radius:999px;
  font-size:12px;
  font-weight:700;
}
.pill-blue{background:rgba(96,165,250,0.16); color:#93c5fd}
.pill-green{background:rgba(52,211,153,0.16); color:#6ee7b7}
.pill-red{background:rgba(248,113,113,0.16); color:#fca5a5}
.pill-amber{background:rgba(251,191,36,0.16); color:#fcd34d}
.note{
  color:var(--muted);
  font-size:12px;
}
.controls{
  padding:20px;
}
.form-group{
  margin-bottom:16px;
}
label{
  display:block;
  margin-bottom:8px;
  font-size:14px;
  color:var(--muted);
  font-weight:700;
}
input{
  width:100%;
  padding:14px 14px;
  border-radius:14px;
  border:1px solid var(--line);
  background:#0b1220;
  color:var(--text);
  font-size:16px;
  outline:none;
}
input:focus{
  border-color:var(--blue);
  box-shadow:0 0 0 3px rgba(96,165,250,0.15);
}
.button{
  width:100%;
  padding:14px 16px;
  border:none;
  border-radius:14px;
  background:linear-gradient(135deg,#2563eb 0%, #3b82f6 100%);
  color:white;
  font-size:16px;
  font-weight:700;
  cursor:pointer;
}
.threshold-list{
  margin-top:16px;
  display:grid;
  gap:10px;
}
.threshold-item{
  display:flex;
  justify-content:space-between;
  gap:10px;
  padding:12px 14px;
  background:var(--panel2);
  border:1px solid var(--line);
  border-radius:14px;
  color:var(--muted);
  font-size:14px;
}
.graph-grid{
  display:grid;
  grid-template-columns:1fr 1fr;
  gap:18px;
  padding:20px;
}
.graph-card{
  background:var(--panel2);
  border:1px solid var(--line);
  border-radius:18px;
  padding:16px;
}
.graph-title{
  font-size:15px;
  font-weight:700;
  margin-bottom:8px;
}
.graph-sub{
  color:var(--muted);
  font-size:12px;
  margin-bottom:12px;
}
.graph-area{
  height:180px;
  display:flex;
  align-items:flex-end;
  gap:4px;
  padding:10px 8px 0 8px;
  background:#0b1220;
  border:1px solid var(--line);
  border-radius:14px;
}
.bar-wrap{
  flex:1;
  height:100%;
  display:flex;
  align-items:flex-end;
}
.bar{
  width:100%;
  border-radius:8px 8px 0 0;
  min-height:2px;
}
.temp-bar{
  background:linear-gradient(180deg,#fb7185 0%, #ef4444 100%);
}
.light-bar{
  background:linear-gradient(180deg,#60a5fa 0%, #2563eb 100%);
}
.graph-footer{
  margin-top:10px;
  color:var(--muted);
  font-size:12px;
  display:flex;
  justify-content:space-between;
  gap:8px;
  flex-wrap:wrap;
}
.footer{
  text-align:center;
  color:var(--muted);
  font-size:13px;
  margin-top:18px;
}
@media (max-width: 900px){
  .main-grid{ grid-template-columns:1fr; }
  .graph-grid{ grid-template-columns:1fr; }
}
@media (max-width: 700px){
  .cards{ grid-template-columns:1fr; }
  .title{ font-size:26px; }
  .hero-left h2{ font-size:22px; }
  .card-value{ font-size:30px; }
  .graph-area{ height:150px; }
}
</style>
<script>
// Reload the page periodically so values and graphs stay current
setTimeout(function() {
  window.location.replace('/');
}, 4000);
</script>
</head>
<body>
<div class="container">
)rawliteral";

  // Top dashboard title and metadata
  html += "<div class='topbar'>";
  html += "<div class='brand'>";
  html += "<div class='title'>ESP32 Smart Monitor</div>";
  html += "<div class='subtitle'>Live environmental dashboard with local trend graphs and saved thresholds</div>";
  html += "</div>";
  html += "<div class='chips'>";
  html += "<div class='chip'>Uptime: " + uptimeString() + "</div>";
  html += "<div class='chip'>Auto refresh: 4s</div>";
  html += "</div>";
  html += "</div>";

  // Hero / overview section
  html += "<div class='hero'><div class='hero-row'><div class='hero-left'>";
  html += "<h2>System Overview</h2>";
  html += "<p>Monitor temperature, humidity, and ambient light in real time. The dashboard now includes local history graphs stored directly on the ESP32.</p>";
  html += "</div>";

  // Current alert state
  html += "<div class='status-box'>";
  html += "<div class='status-label'>Current System Status</div>";
  html += alertState ? "<div class='status-main status-on'>ALERT ON</div>" : "<div class='status-main status-off'>NORMAL</div>";
  html += "<div class='status-reason'>" + getAlertReason() + "</div>";
  html += "</div></div></div>";

  html += "<div class='main-grid'>";

  /******************** LEFT COLUMN ***************************/
  html += "<div class='left-col'>";

  // Live sensor cards
  html += "<div class='section'>";
  html += "<div class='section-header'><h3 class='section-title'>Live Sensor Readings</h3>";
  html += "<div class='section-sub'>Real-time values from the connected environmental sensors</div></div>";

  html += "<div class='cards'>";

  // Temperature card
  html += "<div class='card'><div class='card-label'>Temperature</div>";
  html += "<div class='card-value'>" + String(temperature, 1) + "<span class='card-unit'>&deg;C</span></div>";
  html += "<div class='card-foot'>";
  html += "<span class='pill " + String((temperature >= tempThreshold) ? "pill-red" : "pill-green") + "'>" + getTempLabel() + "</span>";
  html += "<span class='note'>DHT11 sensor</span></div></div>";

  // Humidity card
  html += "<div class='card'><div class='card-label'>Humidity</div>";
  html += "<div class='card-value'>" + String(humidity, 1) + "<span class='card-unit'>%</span></div>";
  html += "<div class='card-foot'><span class='pill pill-blue'>Relative Humidity</span><span class='note'>DHT11 sensor</span></div></div>";

  // Light card
  html += "<div class='card'><div class='card-label'>Light Level</div>";
  html += "<div class='card-value'>" + String(lightValue) + "</div>";
  html += "<div class='card-foot'>";
  html += "<span class='pill " + String((lightValue < lightThreshold) ? "pill-amber" : "pill-green") + "'>" + getLightLabel() + "</span>";
  html += "<span class='note'>Photoresistor ADC</span></div></div>";

  html += "</div></div>";

  // Trend graph section
  html += "<div class='section'>";
  html += "<div class='section-header'><h3 class='section-title'>Recent Trends</h3>";
  html += "<div class='section-sub'>Last 20 local readings stored directly on the ESP32</div></div>";

  html += "<div class='graph-grid'>";

  // Temperature graph
  html += "<div class='graph-card'>";
  html += "<div class='graph-title'>Temperature Trend</div>";
  html += "<div class='graph-sub'>Recent temperature history</div>";
  html += "<div class='graph-area'>" + buildTemperatureBars() + "</div>";
  html += "<div class='graph-footer'><span>Oldest → Newest</span><span>Current: " + String(temperature, 1) + " &deg;C</span></div>";
  html += "</div>";

  // Light graph
  html += "<div class='graph-card'>";
  html += "<div class='graph-title'>Light Trend</div>";
  html += "<div class='graph-sub'>Recent ambient light history</div>";
  html += "<div class='graph-area'>" + buildLightBars() + "</div>";
  html += "<div class='graph-footer'><span>Oldest → Newest</span><span>Current: " + String(lightValue) + "</span></div>";
  html += "</div>";

  html += "</div></div>";

  html += "</div>";

  /******************** RIGHT COLUMN **************************/
  html += "<div class='right-col'><div class='section'>";
  html += "<div class='section-header'><h3 class='section-title'>Threshold Controls</h3>";
  html += "<div class='section-sub'>Changes are saved permanently in ESP32 memory</div></div>";

  html += "<div class='controls'><form action='/' method='GET'>";

  // Temperature threshold input
  html += "<div class='form-group'><label for='temp'>Temperature Threshold (&deg;C)</label>";
  html += "<input type='number' step='0.1' id='temp' name='temp' value='" + String(tempThreshold, 1) + "'></div>";

  // Light threshold input
  html += "<div class='form-group'><label for='light'>Light Threshold</label>";
  html += "<input type='number' id='light' name='light' value='" + String(lightThreshold) + "'></div>";

  // Submit button
  html += "<button class='button' type='submit'>Save Thresholds</button></form>";

  // Summary of saved values
  html += "<div class='threshold-list'>";
  html += "<div class='threshold-item'><span>Saved Temp Threshold</span><strong>" + String(tempThreshold, 1) + " &deg;C</strong></div>";
  html += "<div class='threshold-item'><span>Saved Light Threshold</span><strong>" + String(lightThreshold) + "</strong></div>";
  html += "<div class='threshold-item'><span>Alert Output</span><strong>" + String(alertState ? "LED + Buzzer ON" : "LED + Buzzer OFF") + "</strong></div>";
  html += "</div></div></div></div>";

  // Footer
  html += "<div class='footer'>ESP32 environmental monitoring system • local graphs enabled</div>";
  html += "</div></body></html>";

  return html;
}

/******************** HTTP RESPONSE ***************************/

// Sends the generated dashboard page to the connected browser client
void sendDashboard(WiFiClient& client) {
  String html = buildDashboardPage();

  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: text/html; charset=utf-8");
  client.println("Cache-Control: no-store, no-cache, must-revalidate, max-age=0");
  client.println("Pragma: no-cache");
  client.println("Expires: 0");
  client.print("Content-Length: ");
  client.println(html.length());
  client.println("Connection: close");
  client.println();
  client.print(html);
}

/******************** SETUP ***********************************/

void setup() {
  Serial.begin(115200);
  delay(1000);

  // Initialize hardware interfaces
  dht.begin();
  pinMode(LEDPIN, OUTPUT);
  pinMode(BUZZERPIN, OUTPUT);

  // Initialize dashboard graph history and load saved thresholds
  initializeHistory();
  loadSavedSettings();

  // Connect ESP32 to Wi-Fi network
  Serial.print("Connecting to WiFi: ");
  Serial.println(ssid);

  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();
  Serial.println("Connected!");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());

  // Start web server after network connection is established
  server.begin();
}

/******************** MAIN LOOP *******************************/

void loop() {
  // Keep sensor readings and outputs updated
  readSensors();

  // Check for a new browser client connection
  WiFiClient client = server.available();
  if (client) {
    unsigned long timeout = millis();

    // Wait briefly for request data to arrive
    while (client.connected() && !client.available() && millis() - timeout < 1000) {
      delay(1);
    }

    if (client.available()) {
      // Read first request line and process parameters
      String request = client.readStringUntil('\r');
      client.flush();

      handleThresholdUpdate(request);
      sendDashboard(client);
    }

    // Small delay before closing connection for stability
    delay(5);
    client.stop();
  }
}