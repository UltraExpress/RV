/* Combined RV Thermostat Control System
 * VERSION 12.2
 * Features:
 * - Temperature + humidity (BME280)
 * - WiFi setup (AP mode if no creds)
 * - Web interface with:
 *    * Freeze Guard overlay (touch-hold to enable/disable at 38deg)
 *    * Real-time data endpoint (/data)
 *    * Brightness control with dimming
 *    * Advanced styling + animations
 * - OLED display (Adafruit SSD1306)
 * - Heater control with target + freeze guard hysteresis
 */

//------------------------------------------------------------------------------
// Libraries
//------------------------------------------------------------------------------
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_BME280.h>
#include <WiFi.h>
#include <WebServer.h>
#include <EEPROM.h>

//------------------------------------------------------------------------------
// Definitions
//------------------------------------------------------------------------------
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define SCREEN_ADDRESS 0x3C

#define SEALEVELPRESSURE_HPA (1013.25)

#define HEATER_PIN 4
#define SETUP_BUTTON_PIN 0  // BOOT button

#define EEPROM_SIZE 512
#define SSID_START 0
#define PASS_START 32
#define SSID_MAX_LENGTH 32
#define PASS_MAX_LENGTH 64
#define FREEZE_GUARD_ADDR 96  // store freeze guard on/off

#define DISPLAY_UPDATE_INTERVAL 100
#define WIFI_DISPLAY_DURATION 4000
#define HEADER_TOGGLE_INTERVAL 4000
#define STATE_DISPLAY_DURATION 4000
#define SENSOR_READ_INTERVAL 2000
#define DISPLAY_TOGGLE_INTERVAL 4000


#define UP_BUTTON_PIN 13      // GPIO13 for temp up/heat control
#define DOWN_BUTTON_PIN 14    // GPIO14 for temp down/freeze guard


// Button timing definitions
#define SHORT_PRESS_MS 50      // Debounce time
#define LONG_PRESS_MS 1000     // 1 second for heat/freeze toggles
#define WIFI_RESET_MS 5000     // 5 seconds for WiFi reset
#define SLEEP_PRESS_MS 2000     // 2 second hold for sleep mode

const float FREEZE_GUARD_TEMP = 38.0; // 38deg
const char* www_username = "admin";
const char* www_password = "rv2024";

//------------------------------------------------------------------------------
// Globals
//------------------------------------------------------------------------------
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
Adafruit_BME280 bme;
WebServer server(80);

float currentTemp = 0.0;
float targetTemp  = 72.0;
float humidity    = 0.0;
bool heaterEnabled = false;
bool setupMode = false;
bool isAP = false;
bool freezeGuardEnabled = false;

String storedSSID = "";
String storedPassword = "";
String wifiMessage = "";
bool displayInitialized = false;
bool showingWifiConnected = false;
bool showingHumidity = false;
bool showingSSID = true;
bool showingStateChange = false;
String stateChangeMessage = "";
uint8_t displayBrightness = 255;

unsigned long wifiConnectedDisplayTime = 0;
unsigned long headerToggleTime = -4000;
unsigned long stateChangeTime = 0;
unsigned long displayToggleTime = 0;


//smoothing samples of temp reading
#define SAMPLES 10
float tempReadings[SAMPLES];
float humReadings[SAMPLES];
int readIndex = 0;


int ipScrollPosition = 0;
unsigned long lastIpScroll = 0;
#define IP_DISPLAY_WIDTH 15  // Characters that fit before WiFi bars



// Button state tracking
unsigned long upButtonStart = 0;
unsigned long downButtonStart = 0;
bool lastUpState = HIGH;
bool lastDownState = HIGH;
bool longPressHandled = false;  // Prevents multiple triggers during long press
bool displaySleepMode = false;


//tineout for OLED protection
#define DISPLAY_TIMEOUT 300000  // 5 minute until sleep

unsigned long lastActivityTime = 0;
bool autoSleepEnabled = true;



//part2


//------------------------------------------------------------------------------
// Function Prototypes
//------------------------------------------------------------------------------
bool initializeDisplay();
void updateDisplay();
void showSetupScreen();
void showWiFiConnectedScreen();

void loadWiFiCredentials();
void saveWiFiCredentials(String ssid, String password);
void enterSetupMode(String reason);
void setupNormalMode();

void handleRoot();
void handleWiFiSetup();
void handleTarget();
void handleToggle();
void handleBrightness();
void handleReset();
void handleData();
void handleFreezeGuard();

void saveFreezeGuardState(bool enabled);
void loadFreezeGuardState();

//------------------------------------------------------------------------------
// Setup & Loop
//------------------------------------------------------------------------------







void setup() {
  Serial.begin(115200);
  Serial.println("Starting Thermostat...");

  Wire.begin(); // Make sure this is before BME280 init
  
  // I2C Scanner
  byte error, address;
  Serial.println("Scanning I2C addresses...");
  for(address = 1; address < 127; address++ ) {
    Wire.beginTransmission(address);
    error = Wire.endTransmission();
    if (error == 0) {
      Serial.print("I2C device found at address 0x");
      if (address < 16) Serial.print("0");
      Serial.println(address, HEX);
    }
  }

  // Try both common BME280 addresses
// In setup(), replace the BME280 initialization with:

// Set BME280 to forced mode for better reliability
Serial.println("\nInitializing BME280...");
bool status = bme.begin(0x76);
if (!status) {
  Serial.println("Could not find BME280 at 0x76, trying 0x77...");
  status = bme.begin(0x77);
  if (!status) {
    Serial.println("Could not find BME280!");
    while(1);
  }
}
Serial.println("BME280 initialized successfully!");




if (!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
  Serial.println("SSD1306 failed");
  return;
}



// Set the default sensing mode
bme.setSampling(Adafruit_BME280::MODE_NORMAL,
                Adafruit_BME280::SAMPLING_X1, // temperature
                Adafruit_BME280::SAMPLING_X1, // pressure
                Adafruit_BME280::SAMPLING_X1, // humidity
                Adafruit_BME280::FILTER_OFF);




  pinMode(UP_BUTTON_PIN, INPUT_PULLUP);    // GPIO13
  pinMode(DOWN_BUTTON_PIN, INPUT_PULLUP);  // GPIO14



  
  pinMode(HEATER_PIN, OUTPUT);
  digitalWrite(HEATER_PIN, LOW);
  pinMode(SETUP_BUTTON_PIN, INPUT_PULLUP);
  initializeDisplay();
  loadFreezeGuardState();

  loadWiFiCredentials();
  if (storedSSID.length() < 2) {
    enterSetupMode("No saved WiFi");
  } else {
    wifiMessage = "Connecting to: " + storedSSID;
    updateDisplay();
    WiFi.begin(storedSSID.c_str(), storedPassword.c_str());

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 30) {
      delay(500);
      attempts++;
      String dots = "";
      for(int i = 0; i < (attempts % 4); i++) {
        dots += ".";
      }
      wifiMessage = "Connecting to: " + storedSSID + " " + dots;
      updateDisplay();
    }
    if (WiFi.status() == WL_CONNECTED) {
      setupNormalMode();

    } else {
      enterSetupMode("WiFi Connect Failed");
    }
  }
}











// This should be your loop() function
void loop() {

  handleButtons(); 
  if (!setupMode && digitalRead(SETUP_BUTTON_PIN) == LOW) {
    delay(50);
    if (digitalRead(SETUP_BUTTON_PIN) == LOW) {
      saveWiFiCredentials("", "");
      ESP.restart();
    }
  }

  server.handleClient();

  if (!setupMode) {
    unsigned long now = millis();

    if (showingWifiConnected && (now - wifiConnectedDisplayTime >= WIFI_DISPLAY_DURATION)) {
      Serial.println("Transitioning from WiFi screen to main display...");
      showingWifiConnected = false;
      updateDisplay();
    }

    static unsigned long lastDisplayUpdate = 0;
    if (!showingWifiConnected && (now - lastDisplayUpdate >= DISPLAY_UPDATE_INTERVAL)) {
      updateDisplay();
      lastDisplayUpdate = now;
    }






if (autoSleepEnabled && !displaySleepMode && (now - lastActivityTime > DISPLAY_TIMEOUT)) {
  displaySleepMode = true;
  display.clearDisplay();
  display.display();
}








    static unsigned long lastRead = 0;
static bool sensorInit = true;
if (sensorInit) {
  float initTemp = bme.readTemperature() * 9/5 + 32;
  for(int i = 0; i < SAMPLES; i++) {
    tempReadings[i] = initTemp;
    humReadings[i] = bme.readHumidity();
  }
  delay(8000);
  sensorInit = false;
}
if (now - lastRead >= SENSOR_READ_INTERVAL) {
  float newTemp = bme.readTemperature();
  float newHumidity = bme.readHumidity();
  
  if (!isnan(newTemp)) {
    tempReadings[readIndex] = newTemp * 9/5 + 32;
    float sum = 0;
    for(int i = 0; i < SAMPLES; i++) {
      sum += tempReadings[i];
    }
    currentTemp = sum / SAMPLES;
  }
  
  if (!isnan(newHumidity)) {
    humReadings[readIndex] = newHumidity;
    float sum = 0;
    for(int i = 0; i < SAMPLES; i++) {
      sum += humReadings[i];
    }
    humidity = sum / SAMPLES;
  }
  
  readIndex = (readIndex + 1) % SAMPLES;
  
  // Control heater
  if (freezeGuardEnabled) {
    if (currentTemp < FREEZE_GUARD_TEMP) {
      digitalWrite(HEATER_PIN, HIGH);
      heaterEnabled = true;
    } else if (currentTemp >= (FREEZE_GUARD_TEMP + 2.0)) {
      digitalWrite(HEATER_PIN, LOW);
      heaterEnabled = false;
    }
  } else {
    if (heaterEnabled && currentTemp < targetTemp) {
      digitalWrite(HEATER_PIN, HIGH);
    } else {
      digitalWrite(HEATER_PIN, LOW);
    }
  }
  
  lastRead = now;
}
  }
}

//part3


//------------------------------------------------------------------------------
// Display
//------------------------------------------------------------------------------
bool initializeDisplay() {
  if (!displayInitialized) {
    Serial.println("Starting display init...");
    if (!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
      Serial.println("SSD1306 failed");
      return false;
    }
    Serial.println("SSD1306 success");
    display.clearDisplay();
    display.setTextColor(WHITE);
    displayInitialized = true;
  }
  return true;
}

void updateDisplay() {
  if (!initializeDisplay()) return;
  if (displaySleepMode) return;  // Add this line here - early exit if sleeping

  display.clearDisplay();
  display.setTextColor(WHITE);
  display.setTextSize(1);

  if (isAP) {
    static unsigned long lastScroll = 0;
    static int scrollPosition = 0;
    unsigned long now = millis();
    if (now - lastScroll > 100) {
      scrollPosition++;
      if (scrollPosition > wifiMessage.length() * 6) {
        scrollPosition = -128;
      }
      lastScroll = now;
    }
    display.setCursor(-scrollPosition, 0);
    display.print(wifiMessage);
  } else {
    if (millis() - headerToggleTime > HEADER_TOGGLE_INTERVAL) {
      showingSSID = !showingSSID;
      headerToggleTime = millis();
    }


    //part4


display.setCursor(0, 0);
if (showingSSID) {
    display.print("WiFi: ");
    display.print(storedSSID);
} else {
    display.print("IP: ");
    String ip = WiFi.localIP().toString();
    // If IP is too long to fit
    if (ip.length() > IP_DISPLAY_WIDTH) {
        if (millis() - lastIpScroll > 500) {  // Scroll every 500ms
            ipScrollPosition++;
            if (ipScrollPosition > ip.length() - IP_DISPLAY_WIDTH) {
                ipScrollPosition = 0;
            }
            lastIpScroll = millis();
        }
        display.print(ip.substring(ipScrollPosition, ipScrollPosition + IP_DISPLAY_WIDTH));
    } else {
        display.print(ip);
    }
}
    
display.setCursor(95, 0);
    int rssi = WiFi.RSSI();
    // 5 bars for better signal resolution
    int bars = 0;
    if (rssi > -60) bars = 5;
    else if (rssi > -67) bars = 4;
    else if (rssi > -75) bars = 3;
    else if (rssi > -85) bars = 2;
    else bars = 1;
    
    // Draw 5 positions
    if (bars == 5) display.print("|||||");
    else if (bars == 4) display.print("||||.");
    else if (bars == 3) display.print("|||..");
    else if (bars == 2) display.print("||...");
    else display.print("|....");
    // If freeze guard is enabled, show a note
    if (freezeGuardEnabled) {
      display.setCursor(0,16);
      display.println("FREEZE GUARD ON");
    }

    if (showingStateChange && (millis() - stateChangeTime < STATE_DISPLAY_DURATION)) {
  display.setTextSize(3);  // Make it much bigger - size 5
  display.setCursor(0,25);  // Move right 15 pixels, up to 10 pixels
      display.print(stateChangeMessage);
    } else {
      showingStateChange = false;

      if (millis() - displayToggleTime >= DISPLAY_TOGGLE_INTERVAL) {
        showingHumidity = !showingHumidity;
        displayToggleTime = millis();
      }
      display.setTextSize(1);
      display.setCursor(0, 8);
      display.print("TEMP");
      display.setCursor(70, 8);
      display.print("TARGET");



      //part 5



      display.setTextSize(2);
      display.setCursor(0, 25);
      if (showingHumidity) {
        if (!isnan(humidity)) {
          display.print(humidity, 1);
          display.print("%");
        } else {
          display.print("FAULT");
        }
      } else {
        if (!isnan(currentTemp)) {
          display.print(currentTemp, 1);
        } else {
          int phase = (millis() % 6000) / 2000;  // 6 seconds total cycle, 2 seconds each
          if (phase == 0) display.print("FAULT");
          else if (phase == 1) display.print("TEMP");
          else display.print("SNSR");
        }
      }

display.setCursor(70, 25);
      if (freezeGuardEnabled) {
        display.print("38.0");  // Show freeze guard temp when active
      } else {
        display.print(targetTemp, 1);  // Show normal target temp
      }

      display.setTextSize(1);
      display.setCursor(0, 45);
      display.print("HEATER: ");
      display.print(heaterEnabled ? "ON" : "OFF");
    }
  }
  display.display();
}

void showSetupScreen() {
  if (!initializeDisplay()) return;
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 0);  
  display.println("AP MODE - SETUP");

  display.setCursor(0, 16);
  display.println("WiFi: RV-Setup");

  display.setCursor(0, 24);
  display.println("Pass: 12345678");

  display.setCursor(0, 40);
  display.println("Visit 192.168.4.1");
  display.display();
}



//part 6



void showWiFiConnectedScreen() {
  if (!initializeDisplay()) return;
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0,0);
  display.println("WiFi Connected!");
  display.println("SSID: " + storedSSID);
  display.println("IP: " + WiFi.localIP().toString());
  display.display();
}

//------------------------------------------------------------------------------
// WiFi Functions
//------------------------------------------------------------------------------
void loadWiFiCredentials() {
  EEPROM.begin(EEPROM_SIZE);
  storedSSID = "";
  for (int i = 0; i < SSID_MAX_LENGTH; i++) {
    char c = EEPROM.read(SSID_START + i);
    if (c != 0) storedSSID += c;
  }
  storedPassword = "";
  for (int i = 0; i < PASS_MAX_LENGTH; i++) {
    char c = EEPROM.read(PASS_START + i);
    if (c != 0) storedPassword += c;
  }
  EEPROM.end();
}

void saveWiFiCredentials(String ssid, String password) {
  EEPROM.begin(EEPROM_SIZE);
  for (int i = 0; i < SSID_MAX_LENGTH; i++) {
    EEPROM.write(SSID_START + i, (i < ssid.length() ? ssid[i] : 0));
  }
  for (int i = 0; i < PASS_MAX_LENGTH; i++) {
    EEPROM.write(PASS_START + i, (i < password.length() ? password[i] : 0));
  }
  EEPROM.commit();
  EEPROM.end();
}

void enterSetupMode(String reason) {
  Serial.println(reason + " - entering setup mode");
  setupMode = true;
  isAP = true;

  WiFi.disconnect();
  WiFi.mode(WIFI_AP);
  if (WiFi.softAP("RV-Setup", "12345678")) {
    Serial.println("AP Started Successfully");
    Serial.println("AP IP: " + WiFi.softAPIP().toString());

    showSetupScreen();
    wifiMessage = "SETUP MODE - Connect to: RV-Setup (12345678) -> 192.168.4.1";
    server.on("/", handleWiFiSetup);
    server.begin();
  } else {
    Serial.println("AP Failed to Start");
  }
}



//part 7



void setupNormalMode() {
  wifiMessage = "WIFI: " + storedSSID;
  Serial.println("Connected! IP: " + WiFi.localIP().toString());

  showWiFiConnectedScreen();
  showingWifiConnected = true;
  wifiConnectedDisplayTime = millis();

  setupMode = false;
  isAP = false;
  headerToggleTime = millis();
  showingSSID = true;

  server.on("/", handleRoot);
  server.on("/target", handleTarget);
  server.on("/toggle", handleToggle);
  server.on("/reset", handleReset);
  server.on("/brightness", handleBrightness);
  server.on("/data", handleData);
  server.on("/freezeguard", handleFreezeGuard);
  server.begin();
}

//------------------------------------------------------------------------------
// Freeze Guard State
//------------------------------------------------------------------------------
void saveFreezeGuardState(bool enabled) {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.write(FREEZE_GUARD_ADDR, (enabled ? 1 : 0));
  EEPROM.commit();
  EEPROM.end();
}

void loadFreezeGuardState() {
  EEPROM.begin(EEPROM_SIZE);
  freezeGuardEnabled = (EEPROM.read(FREEZE_GUARD_ADDR) == 1);
  EEPROM.end();
}

//------------------------------------------------------------------------------
// Web Handlers
//------------------------------------------------------------------------------
void handleRoot() {
if (!server.authenticate(www_username, www_password)) {
    server.sendHeader("WWW-Authenticate", "Basic realm=\"RV Control Login\"");
    server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    server.sendHeader("Pragma", "no-cache");
    server.sendHeader("Expires", "-1");
    // Simple loading page
    String loadingHtml = "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>";
    loadingHtml += "<style>body{font-family:-apple-system,sans-serif;text-align:center;padding:40px;}</style>";
    loadingHtml += "</head><body><h2>Authentication Required</h2></body></html>";
    server.send(401, "text/html", loadingHtml);
    return;
}



//part 8



// Advanced UI with freeze guard overlay + hold logic
  String html = "<html><head>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1, maximum-scale=1, user-scalable=no'>";
 html += "<style>";
html += "*{box-sizing:border-box;touch-action:manipulation;-webkit-user-select:none;user-select:none;-webkit-touch-callout:none;}";

html += ".s{display:flex;align-items:center;gap:5px;margin:5px;}";
html += ".dot{width:10px;height:10px;border-radius:50%;animation:blink 1s infinite;}";
html += "@keyframes blink{50%{opacity:0.3}}";


html += "body{font-family:-apple-system,BlinkMacSystemFont,sans-serif;margin:0;padding:16px;background:#f0f2f5;}";
html += ".card{background:white;border-radius:20px;padding:24px;margin:0 auto;max-width:500px;box-shadow:0 2px 12px rgba(0,0,0,0.1);}";
html += ".temp{font-size:72px;font-weight:700;margin:24px 0;text-align:center;}";
html += ".humidity{font-size:20px;color:#666;margin:16px 0;text-align:center;}";
html += ".controls{display:flex;align-items:center;justify-content:center;gap:20px;margin:32px 0;}";


html += ".btn{width:60px;height:60px;font-size:32px;border:none;border-radius:12px;color:white;}";
html += ".btn-up{background:#FF3B30;}"; // Red for up
html += ".btn-down{background:#007AFF;}"; // Blue for down

html += ".target{font-size:24px;font-weight:500;text-align:center;}";
html += ".toggle-container{text-align:center;margin:32px 0;}";
html += ".toggle{position:relative;display:inline-block;width:120px;height:60px;}";
html += ".toggle input{opacity:0;width:0;height:0;}";
html += ".slider{position:absolute;cursor:pointer;top:0;left:0;right:0;bottom:0;background:#ccc;transition:.4s;border-radius:34px;}";
html += ".slider:before{content:'';position:absolute;height:52px;width:52px;left:4px;bottom:4px;background:white;transition:.4s;border-radius:50%;}";
html += "input:checked + .slider{background:#FF3B30;} input:checked + .slider:before{transform:translateX(60px);}";
html += ".status{text-align:center;margin-top:8px;font-size:18px;font-weight:500;}";
html += ".brightness-container{text-align:center;margin:32px 0;}";
html += "</style>";

  // Script

html += "<script>";
html += "function debounce(func,wait){let t;return function(...args){clearTimeout(t);t=setTimeout(()=>func.apply(this,args),wait);}}";

html += "function wakeDisplay() {";
html += "  return fetch('/brightness?value=1');";
html += "}";

html += "function updateTemp(change){";
html += "  event.preventDefault();";
html += "  const btn = event.target;";
html += "  if(btn.disabled) return;";
html += "  btn.disabled = true;";
html += "  const targetDiv = document.querySelector('.target');";
html += "  const currentTemp = parseFloat(targetDiv.innerText.match(/\\d+\\.?\\d*/)[0]);";
html += "  const newTemp = Math.min(90, Math.max(50, currentTemp + change));";
html += "  targetDiv.innerHTML = 'Target:<br>' + newTemp.toFixed(1) + '&deg;F';";
html += "  wakeDisplay().then(() => {";  // Wake first
html += "    return fetch('/target?temp='+change);";
html += "  }).then(()=>updateData())";
html += "    .finally(()=>setTimeout(()=>btn.disabled=false,250));";
html += "}";

html += "function toggleHeater(){";
html += "  wakeDisplay().then(() => {";
html += "    return fetch('/toggle');";
html += "  }).then(()=>updateData());";
html += "}";

html += "const updateBrightness=debounce((val)=>{fetch('/brightness?value='+val);},250);";

html += "function toggleFreezeGuard(){";
html += "  const checked=event.target.checked;";
html += "  wakeDisplay().then(() => {";
html += "    return fetch('/freezeguard?state='+(checked?'1':'0'));";
html += "  }).then(()=>updateData());";
html += "}";

html += "function updateData(){const controller=new AbortController();const timeoutId=setTimeout(()=>controller.abort(),2000);fetch('/data',{signal:controller.signal,timeout:1000}).then(r=>r.json()).then(d=>{clearTimeout(timeoutId);";
html += "document.getElementById('d').style.background='#0f0';";
html += "document.getElementById('status').textContent='Connected';";
html += "document.querySelector('.temp').innerHTML = (d.currentTemp === 'TEMP SENSOR FAULT' ? d.currentTemp : d.currentTemp.toFixed(1)+'&deg;F');";
html += "document.querySelector('.humidity').innerHTML='Humidity: '+d.humidity.toFixed(1)+'%';";
html += "const t=document.querySelector('.target');";
html += "if(d.freezeGuardEnabled){";
html += "  t.innerHTML='Target:<br>38.0&deg;F';";
html += "  document.querySelectorAll('.btn').forEach(b=>{b.disabled=true;b.style.opacity='0.5';});";
html += "} else {";
html += "  t.innerHTML='Target:<br>'+d.targetTemp.toFixed(1)+'&deg;F';";
html += "  document.querySelectorAll('.btn').forEach(b=>{b.disabled=false;b.style.opacity='1';});";
html += "}";
html += "document.querySelector('input[onclick=\"toggleFreezeGuard()\"]').checked=d.freezeGuardEnabled;";
html += "document.querySelector('input[onclick=\"toggleHeater()\"]').checked=d.heaterEnabled;";
html += "document.querySelector('#heaterStatus').innerHTML='Heater '+(d.heaterEnabled?'ON':'OFF');";
html += "document.querySelector('#freezeStatus').innerHTML='Freeze Protection '+(d.freezeGuardEnabled?'ON':'OFF');";
html += "document.querySelector('#brightnessLabel').innerHTML = d.displaySleep ? 'SLEEPING' : 'Screen Brightness';";
html += "document.querySelector('input[type=\"range\"]').value = d.brightness;";
html += "}).catch(()=>{";
html += "document.getElementById('d').style.background='red';";
html += "Promise.any([";
html += "fetch('http://192.168.1.1', {mode: 'no-cors', timeout: 500}),";
html += "fetch('http://192.168.0.1', {mode: 'no-cors', timeout: 500}),";
html += "fetch('http://10.0.0.1', {mode: 'no-cors', timeout: 500})";
html += "])";
html += ".then(() => {";
html += "  document.getElementById('status').textContent='Device not found on network';";
html += "})";
html += ".catch(() => {";
html += "  document.getElementById('status').textContent='Check phone WiFi connection';";
html += "})";
html += "});}";
html += "setInterval(updateData,2000);";
html += "</script></head>";


// Body with freeze-guard UI
html += "<body>";
html += "<div class='card'>";

html += "<div class='s'><div id='d' class='dot' style='background:red'></div><span id='status' style='font-size:12px'>Connecting...</span></div>";



html += "<h1 style='font-size:28px;margin:0;text-align:center;'>RV Temperature Control</h1>";
html += "<div class='temp'>" + String(currentTemp,1) + "&deg;F</div>";
html += "<div class='humidity'>Humidity: " + String(humidity,1) + "%</div>";

html += "<div class='controls' style='min-height:120px;'>";
html += "<button class='btn btn-down' onclick='updateTemp(-1)'>-</button>";
html += "<div class='target' style='min-width:100px;'>Target:<br>" + String(targetTemp,1) + "&deg;F</div>";
html += "<button class='btn btn-up' onclick='updateTemp(1)'>+</button>";
html += "</div>";

html += "<div class='toggle-container'>";
html += "<label class='toggle'>";
html += "<input type='checkbox' " + String(heaterEnabled?"checked":"") + " onclick='toggleHeater()'>";
html += "<span class='slider'></span>";
html += "</label>";
html += "<div class='status' id='heaterStatus'>Heater " + String(heaterEnabled?"ON":"OFF") + "</div>";
html += "</div>";

html += "<div class='toggle-container'>";
html += "<div style='margin-bottom:8px;'>Freeze Guard (38deg)</div>";
html += "<label class='toggle'>";
html += "<input type='checkbox' " + String(freezeGuardEnabled?"checked":"") + " onclick='toggleFreezeGuard()'>";
html += "<span class='slider'></span>";
html += "</label>";

html += "<div class='status' id='freezeStatus' style='white-space:nowrap;'>Protection " + String(freezeGuardEnabled?"ON":"OFF") + "</div>";
html += "</div>";

// Find the brightness container section and replace with:
html += "<div class='brightness-container'>";
html += "<label id='brightnessLabel'>Screen Brightness</label><br>";
html += "<input type='range' min='0' max='1' step='0.1' value='" + String((float)displayBrightness/255.0,1) + "' oninput='updateBrightness(this.value)' style='width:200px;'>";
html += "</div>";

html += "<div style='text-align:center;margin-top:32px;'>";
html += "<button onclick='if(confirm(\"Reset WiFi settings?\")){window.location=\"/reset\";}' style='background:#FF3B30;color:white;border:none;border-radius:8px;padding:12px 24px;font-size:16px;'>Reset WiFi Settings</button>";
html += "</div>";

html += "</div></body></html>";

  server.send(200, "text/html", html);
}



//part 11



void handleWiFiSetup() {
  if (!server.authenticate(www_username, www_password)) {
    return server.requestAuthentication();
  }
  if (server.hasArg("ssid") && server.hasArg("password")) {
    String newSSID = server.arg("ssid");
    String newPASS = server.arg("password");
    saveWiFiCredentials(newSSID, newPASS);
    server.send(200, "text/html", "<html><body><h2>WiFi Settings Saved</h2><p>Device will now restart...</p></body></html>");
    delay(2000);
    ESP.restart();
  } else {
    String html = "<html><head>";
    html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
    html += "<style>body{font-family:-apple-system,BlinkMacSystemFont,sans-serif;margin:0;padding:20px;background:#f0f2f5;} .card{background:white;border-radius:20px;padding:24px;margin:0 auto;max-width:500px;box-shadow:0 2px 12px rgba(0,0,0,0.1);} input{width:100%;padding:12px;margin:8px 0;border:1px solid #ddd;border-radius:8px;font-size:16px;} button{width:100%;padding:12px;background:#007AFF;color:white;border:none;border-radius:8px;font-size:16px;margin-top:16px;} </style>";
    html += "</head><body><div class='card'>";
    html += "<h2>WiFi Setup</h2><form method='post'>";
    html += "<div>SSID:</div><input type='text' name='ssid'><br>";
    html += "<div>Password:</div><input type='password' name='password'><br>";
    html += "<button type='submit'>Save & Restart</button>";
    html += "</form></div></body></html>";
    server.send(200,"text/html",html);
  }
}

void handleTarget() {
  if (!server.authenticate(www_username, www_password)) {
    return server.requestAuthentication();
  }
  if (server.hasArg("temp")) {
    float change = server.arg("temp").toFloat();
    targetTemp += change;
    if (targetTemp < 50) targetTemp = 50;
    if (targetTemp > 90) targetTemp = 90;
    showingStateChange = true;
    stateChangeMessage = String(targetTemp,1) + "F";
    stateChangeTime = millis();
  }
  server.send(200, "text/plain", "OK");
}

void handleToggle() {
  if (!server.authenticate(www_username, www_password)) {
    return server.requestAuthentication();
  }
  if (!freezeGuardEnabled) {  // Add this check
    heaterEnabled = !heaterEnabled;
    digitalWrite(HEATER_PIN, heaterEnabled ? HIGH : LOW);
    showingStateChange = true;
    stateChangeMessage = (heaterEnabled ? "HEAT ON" : "HEATOFF");
    stateChangeTime = millis();
  }
  server.send(200, "text/plain", "OK");
}


//part 12



void handleBrightness() {
  if (!server.authenticate(www_username, www_password)) {
    return server.requestAuthentication();
  }
  if (server.hasArg("value")) {
    float val = server.arg("value").toFloat();
    if (val == 0) {
      displaySleepMode = true;
      display.clearDisplay();
      display.display();
    } else {
      displaySleepMode = false;
      lastActivityTime = millis();
      displayBrightness = (uint8_t)(val * 255.0);
      display.dim(false);  // Force full brightness on wake
      updateDisplay();     // Force display update
    }
  }
  server.send(200, "text/plain", "OK");
}

void handleReset() {
  if (!server.authenticate(www_username, www_password)) {
    return server.requestAuthentication();
  }
  String html = "<html><head><meta name='viewport' content='width=device-width, initial-scale=1'></head><body>";
  html += "<h2>Device is resetting...</h2>";
  html += "<p>Restarting in setup mode.</p></body></html>";
  server.send(200,"text/html",html);

  delay(2000);
  saveWiFiCredentials("", "");
  ESP.restart();
}

void handleData() {
  if (!server.authenticate(www_username, www_password)) {
    return server.requestAuthentication();
  }
  
  String json = "{";
  json += "\"currentTemp\":" + (isnan(currentTemp) ? "\"TEMP SENSOR FAULT\"" : String(currentTemp,1)) + ",";
  json += "\"humidity\":" + String(humidity,1) + ",";
  json += "\"targetTemp\":" + String(targetTemp,1) + ",";
  json += "\"heaterEnabled\":";
  json += (heaterEnabled ? "true" : "false");
  json += ",";
  json += "\"freezeGuardEnabled\":";
  json += (freezeGuardEnabled ? "true" : "false");
  json += ",\"displaySleep\":";
  json += (displaySleepMode ? "true" : "false");
  json += ",\"brightness\":";
  json += (displaySleepMode ? "0" : String((float)displayBrightness/255.0,1));
  json += "}";

  server.send(200, "application/json", json);
}



void handleButtons() {
    bool currentUpState = digitalRead(UP_BUTTON_PIN);    // GPIO13
    bool currentDownState = digitalRead(DOWN_BUTTON_PIN); // GPIO14
    unsigned long now = millis();
    
    // Only update activity time when buttons are pressed
    if (currentUpState == LOW || currentDownState == LOW) {
        lastActivityTime = now;
    }

    // Handle Wake from Sleep First
    if (displaySleepMode && (currentUpState == LOW || currentDownState == LOW)) {
        displaySleepMode = false;
        display.dim(false);
        showingStateChange = true;
        stateChangeMessage = "WAKE";
        stateChangeTime = now;
        delay(250); // Debounce wake
        return;     // Exit to prevent unwanted actions
    }

    // Regular Button Handling (only if not in sleep mode)
    if (!displaySleepMode) {
        // UP Button Handler
        if (currentUpState != lastUpState) {
            if (currentUpState == LOW) {  // Button just pressed
                upButtonStart = now;
                longPressHandled = false;
            } else {  // Button just released
                if (now - upButtonStart < LONG_PRESS_MS) {
                    // Short press - increase temperature
                    if (!freezeGuardEnabled) {
                        targetTemp = min(90.0, targetTemp + 1.0);
                        showingStateChange = true;
                        stateChangeMessage = String(targetTemp, 1) + "F";
                        stateChangeTime = now;
                    }
                }
            }
            lastUpState = currentUpState;
        }

// UP long press check - Heat On/Off
if (currentUpState == LOW && !longPressHandled && 
    (now - upButtonStart >= LONG_PRESS_MS)) {
    if (!freezeGuardEnabled) {  // Add this check
        heaterEnabled = !heaterEnabled;
        digitalWrite(HEATER_PIN, heaterEnabled ? HIGH : LOW);
        showingStateChange = true;
        stateChangeMessage = heaterEnabled ? "HEAT ON" : "HEATOFF";
        stateChangeTime = now;
    }
    longPressHandled = true;
}

        // DOWN Button Handler
        if (currentDownState != lastDownState) {
            if (currentDownState == LOW) {  // Button just pressed
                downButtonStart = now;
                longPressHandled = false;
            } else {  // Button just released
                if (now - downButtonStart < LONG_PRESS_MS) {
                    // Short press - decrease temperature
                    if (!freezeGuardEnabled) {
                        targetTemp = max(50.0, targetTemp - 1.0);
                        showingStateChange = true;
                        stateChangeMessage = String(targetTemp, 1) + "F";
                        stateChangeTime = now;
                    }
                }
            }
            lastDownState = currentDownState;
        }

        // DOWN long press check - Freeze Guard
        if (currentDownState == LOW && !longPressHandled && 
            (now - downButtonStart >= LONG_PRESS_MS)) {
            freezeGuardEnabled = !freezeGuardEnabled;
            saveFreezeGuardState(freezeGuardEnabled);
            showingStateChange = true;
            stateChangeMessage = freezeGuardEnabled ? "GRD ON" : "GRD OFF";
            stateChangeTime = now;
            longPressHandled = true;
        }

        // Sleep Mode Check - both buttons for 2 seconds
        if (currentUpState == LOW && currentDownState == LOW) {
            if (now - upButtonStart >= SLEEP_PRESS_MS && 
                now - downButtonStart >= SLEEP_PRESS_MS &&
                now - upButtonStart < WIFI_RESET_MS) {
                
                displaySleepMode = true;
                showingStateChange = true;
                stateChangeMessage = "SLEEP";
                stateChangeTime = now;
                display.dim(true);
                display.clearDisplay();
                display.display();
                delay(250); // Prevent immediate wake
                return;
            }
        }

        // WiFi Reset - both buttons for 5 seconds
        if (currentUpState == LOW && currentDownState == LOW) {
            if (now - upButtonStart >= WIFI_RESET_MS && 
                now - downButtonStart >= WIFI_RESET_MS) {
                showingStateChange = true;
                stateChangeMessage = "RESET";
                stateChangeTime = now;
                delay(1000);
                saveWiFiCredentials("", "");
                ESP.restart();
            }
        }
    }
}






void handleFreezeGuard() {
  if (!server.authenticate(www_username, www_password)) {
    return server.requestAuthentication();
  }
  if (server.hasArg("state")) {
    freezeGuardEnabled = (server.arg("state") == "1");
    saveFreezeGuardState(freezeGuardEnabled);
    showingStateChange = true;
    stateChangeMessage = (freezeGuardEnabled ? "GRD ON" : "GRD OFF");
    stateChangeTime = millis();
  }
  server.send(200, "text/plain", "OK");
}