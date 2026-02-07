// ---------- Configuration and Libraries ----------
#include "app_secrets.h"
#include <SPI.h>
#include <MFRC522.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <LiquidCrystal_I2C.h>
#include <NTPClient.h>
#include <WiFiS3.h>
#include <WiFiUdp.h>
#include "WiFiSSLClient.h"
#include "RTC.h"
#include <Wire.h>

#define RST_PIN 9
#define SS_PIN 10

#define GMTOffset_hour 8
#define DayLightSaving 0

MFRC522 rfid(SS_PIN, RST_PIN);
LiquidCrystal_I2C lcd(0x27, 16, 2);
WiFiSSLClient wifiClient;
PubSubClient mqttClient(wifiClient);
WiFiUDP Udp;
NTPClient timeClient(Udp);

const char WIFI_SSID[] = SECRET_WIFI_SSID;
const char WIFI_PASS[] = SECRET_WIFI_PASS;

const int   MQTT_PORT   = SECRET_MQTT_PORT;
const char* MQTT_BROKER = SECRET_MQTT_BROKER;
const char* MQTT_USERNAME = SECRET_MQTT_USERNAME;
const char* MQTT_PASSWORD = SECRET_MQTT_PASSWORD;

const char* MQTT_ATTENDANCE_TOPIC   = SECRET_MQTT_ATTENDANCE_TOPIC;   
const char* MQTT_REGISTRATION_TOPIC = SECRET_MQTT_REGISTRATION_TOPIC; 
const char* MQTT_STATUS_TOPIC       = SECRET_MQTT_STATUS_TOPIC;       
const char* MQTT_MODE_TOPIC         = SECRET_MQTT_MODE_TOPIC;         
const char* MQTT_FEEDBACK_TOPIC     = SECRET_MQTT_FEEDBACK_TOPIC;     

const String MASTER_CARD = SECRET_MASTER_CARD;

const byte WIFI_CONNECTED_LED     = 8;
const byte RFID_READ_LED          = 7;
const byte REGISTRATION_MODE_LED  = 6;
const byte BUZZER_PIN             = 4;

const char* DEVICE_ID   = "READER-01";
const char* DEVICE_NAME = "Reader Prototype";

enum SystemMode {
  IDLE,
  ATTENDANCE,
  REGISTRATION,
  FEEDBACK,
  ERROR_DISPLAY  // New mode for showing errors/success messages
};

SystemMode systemMode = SystemMode::ATTENDANCE;
String feedbackMessage = "Waiting for card";

// --- Debounce and Lockout ---
String lastDeniedCard = "";
unsigned long lastDeniedTime = 0;
const unsigned long debounceDelay = 3000;      // 3 seconds debounce for display

String lastScannedCard = "";
unsigned long lastScannedTime = 0;
const unsigned long scanDebounceDelay = 2000;  // 2 seconds debounce for scan

// Feedback de-duplication (prevents loop/echo)
String lastFeedbackKey = "";
unsigned long lastFeedbackTime = 0;
const unsigned long feedbackDebounceDelay = 2500; // ignore duplicate feedback within 2.5s

// Lockout after error event
bool errorLockout = false;
unsigned long errorLockoutEnd = 0;
const unsigned long errorLockoutTime = 5000;   // 5 seconds lockout after error

// Error display timing
unsigned long errorDisplayStart = 0;
const unsigned long errorDisplayTime = 3000;   // Show error/success for 3 seconds

// ---------- Function Prototypes ----------
void connectToWiFi();
void connectToMQTTBroker();
void printWiFiStatus();
String getTimestamp();
void printDiscoveryJsonOnce();
void updateDisplay();
void normalBeep();
void masterBeep();
void errorBeep();
bool appDelay(unsigned long interval);
void displayRegistrationScreen();
void displayDateTime();
void dataCallback(char *topic, byte *payload, unsigned int length);

// ---------- Setup ----------
void setup() {
  Serial.begin(115200);
  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.print("Initializing...");

  WiFi.begin(WIFI_SSID, WIFI_PASS);
  SPI.begin();
  rfid.PCD_Init();
  RTC.begin();

  pinMode(WIFI_CONNECTED_LED, OUTPUT);
  pinMode(RFID_READ_LED, OUTPUT);
  pinMode(REGISTRATION_MODE_LED, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, HIGH);

  appDelay(1000);
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Connecting to");
  lcd.setCursor(0, 1);
  lcd.print("WiFi...");

  connectToWiFi();
  appDelay(1000);

  lcd.clear();
  lcd.print("Setting clock...");
  timeClient.begin();
  timeClient.update();
  {
    unsigned long epochTime = timeClient.getEpochTime();
    auto unixTime = epochTime + (GMTOffset_hour * 3600);
    RTCTime timeToSet = RTCTime(unixTime);
    RTC.setTime(timeToSet);
  }
  appDelay(1000);

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Connecting to");
  lcd.setCursor(0, 1);
  lcd.print("Server...");

  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
  mqttClient.setCallback(dataCallback);
  connectToMQTTBroker();

  appDelay(1000);
  lcd.clear();
  lcd.print("System ready!");
  appDelay(1000);
  lcd.clear();

  printDiscoveryJsonOnce();
  systemMode = SystemMode::ATTENDANCE;
  digitalWrite(REGISTRATION_MODE_LED, LOW);
  digitalWrite(RFID_READ_LED, HIGH);
}

// ---------- Main Loop ----------
void loop() {
  digitalWrite(BUZZER_PIN, HIGH);

  if (!mqttClient.connected()) connectToMQTTBroker();
  mqttClient.loop();
  updateDisplay();

  // Check if error lockout is active and if it's over
  if (errorLockout && millis() > errorLockoutEnd) {
    errorLockout = false;
  }
  if (errorLockout) {
    // Don't read or process scans during lockout
    return;
  }

  // FIXED: Check if error display time is over and properly reset
  if (systemMode == SystemMode::ERROR_DISPLAY && millis() > errorDisplayStart + errorDisplayTime) {
    systemMode = SystemMode::ATTENDANCE;
    feedbackMessage = "Waiting for card";
    lcd.clear();  // Clear for safety
    Serial.println("🔄 Returning to ATTENDANCE mode");
  }

  String timestamp = getTimestamp();

  String cardID = "";
  if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
    for (byte i = 0; i < rfid.uid.size; i++) {
      cardID.concat(String(rfid.uid.uidByte[i], HEX));
    }
    cardID.toUpperCase();
    rfid.PICC_HaltA();
  }

  if (cardID != "") {
    unsigned long now = millis();
    if (cardID != lastScannedCard || (now - lastScannedTime > scanDebounceDelay)) {
      lastScannedCard = cardID;
      lastScannedTime = now;

      if (cardID == MASTER_CARD) {
        Serial.println("🔐 Master card detected!");
        masterBeep();

        if (systemMode != SystemMode::REGISTRATION) {
          systemMode = SystemMode::REGISTRATION;
          digitalWrite(REGISTRATION_MODE_LED, HIGH);
          digitalWrite(RFID_READ_LED, LOW);
          Serial.println("🔄 Switching to REGISTRATION mode");
        } else {
          systemMode = SystemMode::ATTENDANCE;
          digitalWrite(REGISTRATION_MODE_LED, LOW);
          digitalWrite(RFID_READ_LED, HIGH);
          Serial.println("🔄 Switching to ATTENDANCE mode");
        }

        // Publish mode change
        JsonDocument modeDoc;
        modeDoc["mode"] = (systemMode == SystemMode::REGISTRATION) ? "registration" : "attendance";
        String modePayload;
        serializeJson(modeDoc, modePayload);
        mqttClient.publish(MQTT_MODE_TOPIC, modePayload.c_str());

        Serial.println("📤 Publishing to: " + String(MQTT_MODE_TOPIC));
        Serial.println("📦 Payload: " + modePayload);

        printDiscoveryJsonOnce();

        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print(systemMode == SystemMode::ATTENDANCE ? "Back to Attendance" : "Registration");
        lcd.setCursor(0, 1);
        lcd.print("RFID: Admin Card");
      } else {
        // DON'T beep immediately - wait for feedback
        // normalBeep(); // REMOVED THIS LINE

        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("Card Scanned");
        lcd.setCursor(0, 1);
        lcd.print("Verifying...");

        JsonDocument doc;
        doc["rfid"] = cardID;
        doc["timestamp"] = timestamp;
        doc["deviceId"] = DEVICE_ID;
        doc["mode"] = (systemMode == SystemMode::REGISTRATION) ? "registration" : "attendance";

        String payload;
        serializeJson(doc, payload);

        if (systemMode == SystemMode::REGISTRATION) {
          mqttClient.publish(MQTT_REGISTRATION_TOPIC, payload.c_str());
        } else {
          mqttClient.publish(MQTT_ATTENDANCE_TOPIC, payload.c_str());
        }

        Serial.println("📤 Sent scan:");
        Serial.println(payload);
      }
    } else {
      Serial.println("Duplicate card scan ignored.");
    }
  }
}

// ---------- Supporting Functions ----------
void connectToWiFi() {
  const unsigned long blinkInterval = 500;
  unsigned long previousMillis = 0;
  bool ledState = LOW;

  while (WiFi.status() != WL_CONNECTED) {
    Serial.println("Connecting to WiFi...");
    unsigned long currentMillis = millis();
    if (currentMillis - previousMillis >= blinkInterval) {
      previousMillis = currentMillis;
      ledState = !ledState;
      digitalWrite(WIFI_CONNECTED_LED, ledState);
    }
    delay(10);
  }

  Serial.println("Connected to WiFi");
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Connected to");
  lcd.setCursor(0, 1);
  lcd.print("WiFi Network");
  digitalWrite(WIFI_CONNECTED_LED, HIGH);
  printWiFiStatus();
}
void printWiFiStatus() {
  Serial.print("SSID: "); Serial.println(WiFi.SSID());
  IPAddress ip = WiFi.localIP();
  Serial.print("IP Address: "); Serial.println(ip);
  long rssi = WiFi.RSSI();
  Serial.print("Signal Strength (RSSI): "); Serial.print(rssi); Serial.println(" dBm");
}
void blinkLed(byte ledPin, byte blinkDelay) {
  for (byte times = 0; times < 3; times++) {
    digitalWrite(ledPin, HIGH); delay(blinkDelay);
    digitalWrite(ledPin, LOW); delay(blinkDelay);
  }
}
String getTimestamp() {
  RTCTime currentTime; RTC.getTime(currentTime);
  return currentTime.toString();
}
void connectToMQTTBroker() {
  while (!mqttClient.connected()) {
    String client_id = "attendance-system-client";
    Serial.println("Connecting to MQTT Broker...");
    if (mqttClient.connect(client_id.c_str(), MQTT_USERNAME, MQTT_PASSWORD)) {
      Serial.println("Connected to MQTT broker");

      // Subscribe only to topics this device must react to
      mqttClient.subscribe(MQTT_STATUS_TOPIC);
      mqttClient.subscribe(MQTT_MODE_TOPIC);
      mqttClient.subscribe(MQTT_FEEDBACK_TOPIC); // feedback is the only one that updates UI/beeper directly

      Serial.println("Subscribed to topics.");

      JsonDocument doc;
      doc["status"] = "connected";
      String statusMessage;
      serializeJson(doc, statusMessage);
      mqttClient.publish(MQTT_STATUS_TOPIC, statusMessage.c_str(), true);

      digitalWrite(RFID_READ_LED, HIGH);
    } else {
      Serial.print("Failed to connect to MQTT broker, rc="); Serial.println(mqttClient.state());
    }
  }
}
String twoDigits(int number) {
  if (number < 10) return "0" + String(number);
  return String(number);
}
void displayDateTime() {
  RTCTime currentTime; RTC.getTime(currentTime);
  String month = twoDigits(Month2int(currentTime.getMonth()));
  String day   = twoDigits(currentTime.getDayOfMonth());
  String year  = String(currentTime.getYear()).substring(2);
  String hour  = twoDigits(currentTime.getHour() % 12);
  String mins  = twoDigits(currentTime.getMinutes());
  String datetimeStr = month + "-" + day + "-" + year + "  " + hour + ":" + mins;
  lcd.setCursor(0, 0); lcd.print(datetimeStr);
}
void displayRegistrationScreen() {
  lcd.setCursor(0, 0); lcd.print("Registration");
  lcd.setCursor(0, 1); lcd.print("Tap the card...");
}
bool appDelay(unsigned long interval) {
  unsigned long now = millis();
  while (!((millis() - now) > interval)) { /* wait */ }
  return true;
}

// Topic-aware, debounced callback to prevent feedback loops
void dataCallback(char *topic, byte *payload, unsigned int length) {
  // Copy payload into a null-terminated buffer
  char payloadStr[length + 1];
  memset(payloadStr, 0, length + 1);
  strncpy(payloadStr, (char *)payload, length);

  // Log raw for diagnostics
  Serial.print("📨 MQTT ["); Serial.print(topic); Serial.print("] ");
  Serial.println(payloadStr);

  // Only react to feedback and mode/status topics
  bool isFeedback = (strcmp(topic, MQTT_FEEDBACK_TOPIC) == 0);
  bool isMode     = (strcmp(topic, MQTT_MODE_TOPIC) == 0);
  bool isStatus   = (strcmp(topic, MQTT_STATUS_TOPIC) == 0);

  if (!isFeedback && !isMode && !isStatus) {
    return; // ignore unrelated topics entirely
  }

  JsonDocument doc;
  DeserializationError jerr = deserializeJson(doc, payloadStr);
  if (jerr) {
    Serial.print("❌ JSON parse error: "); Serial.println(jerr.c_str());
    return;
  }

  if (isMode) {
    String modeStr = doc["mode"] | "";
    if (modeStr == "registration") {
      systemMode = SystemMode::REGISTRATION;
      digitalWrite(REGISTRATION_MODE_LED, HIGH);
      digitalWrite(RFID_READ_LED, LOW);
    } else if (modeStr == "attendance") {
      systemMode = SystemMode::ATTENDANCE;
      digitalWrite(REGISTRATION_MODE_LED, LOW);
      digitalWrite(RFID_READ_LED, HIGH);
    }
    return;
  }

  if (isStatus) {
    // Optionally handle status heartbeats here; currently ignore
    return;
  }

  // FEEDBACK processing below
  String message = doc["message"] | "";
  String status  = doc["status"]  | "";   // may be empty
  String rfidVal = doc["value"]   | "";

  // Normalize status if backend only sent message
  if (status.length() == 0 && message.length() > 0) {
    if (message == "Error") status = "error";
    else if (message == "Not on schedule") status = "not_on_schedule";
    else if (message == "Recorded!" || message == "Already scanned") status = "recognized";
  }

  // Debounce duplicate feedback (status + rfid)
  String feedbackKey = status + "|" + rfidVal;
  unsigned long now = millis();
  if (feedbackKey == lastFeedbackKey && (now - lastFeedbackTime) < feedbackDebounceDelay) {
    Serial.println("⏭️  Duplicate feedback ignored");
    return;
  }
  lastFeedbackKey = feedbackKey;
  lastFeedbackTime = now;

  Serial.print("📩 Received feedback: "); Serial.println(message);
  if (rfidVal != "") { Serial.print("🔑 RFID: "); Serial.println(rfidVal); }
  Serial.print("🔍 Parsed status: '"); Serial.print(status); Serial.println("'");

  lcd.clear();
  lcd.setCursor(0, 0);

  if (status == "error") {
    lcd.print("Service Error");
    lcd.setCursor(0, 1);
    lcd.print("Please try again");
    errorBeep();
    errorLockout = true;
    errorLockoutEnd = millis() + errorLockoutTime;
    systemMode = SystemMode::ERROR_DISPLAY;
    errorDisplayStart = millis();
    Serial.println("🔄 Set to ERROR_DISPLAY mode");
    return; // exit to avoid further changes this cycle
  }
  else if (status == "unrecognized") {
    lcd.print("Card Denied");
    lcd.setCursor(0, 1);
    lcd.print("Invalid Card");
    errorBeep();
    errorLockout = true;
    errorLockoutEnd = millis() + errorLockoutTime;
    systemMode = SystemMode::ERROR_DISPLAY;
    errorDisplayStart = millis();
    Serial.println("🔄 Set to ERROR_DISPLAY mode");
  }
  else if (status == "recognized" && message == "Already scanned") {
    lcd.print("Already scanned");
    lcd.setCursor(0, 1); lcd.print(rfidVal);
    normalBeep();
    systemMode = SystemMode::ERROR_DISPLAY;
    errorDisplayStart = millis();
    Serial.println("🔄 Set to ERROR_DISPLAY mode");
  }
  else if (status == "recognized") {
    lcd.print("Recorded!");
    lcd.setCursor(0, 1); lcd.print(rfidVal);
    normalBeep();
    systemMode = SystemMode::ERROR_DISPLAY;
    errorDisplayStart = millis();
    Serial.println("🔄 Set to ERROR_DISPLAY mode");
  }
  else if (status == "not_on_schedule") {
    lcd.print("Not on schedule");
    lcd.setCursor(0, 1); lcd.print(rfidVal);
    errorBeep();
    errorLockout = true;
    errorLockoutEnd = millis() + errorLockoutTime;
    systemMode = SystemMode::ERROR_DISPLAY;
    errorDisplayStart = millis();
    Serial.println("🔄 Set to ERROR_DISPLAY mode");
  }
  else {
    lcd.print(message.length() ? message : "Processing...");
    systemMode = SystemMode::ERROR_DISPLAY;
    errorDisplayStart = millis();
    Serial.println("🔄 Set to ERROR_DISPLAY mode");
  }
}

// FIXED: updateDisplay function without blocking delays
void updateDisplay() {
  if (systemMode == SystemMode::ERROR_DISPLAY) {
    // Don't overwrite error/success messages - they handle their own display
    // The error display will automatically switch back to ATTENDANCE mode after timeout
    return;
  }
  else if (systemMode == SystemMode::ATTENDANCE) {
    displayDateTime();
    lcd.setCursor(0, 1);
    lcd.print(feedbackMessage);
  }
  else if (systemMode == SystemMode::REGISTRATION) {
    displayRegistrationScreen();
  }
  else if (systemMode == SystemMode::IDLE) {
    // Handle IDLE mode - show waiting message
    lcd.setCursor(0, 0);
    lcd.print("System Ready");
    lcd.setCursor(0, 1);
    lcd.print("Waiting for card");
  }
  else if (systemMode == SystemMode::FEEDBACK) {
    displayDateTime();
    lcd.setCursor(0, 1);
    lcd.print(feedbackMessage);
    // REMOVED delay(1000) - this was blocking the system!
    // REMOVED systemMode change - let the main loop handle it
  }
}
void printDiscoveryJsonOnce() {
  JsonDocument jd;
  jd["deviceId"] = DEVICE_ID;
  jd["deviceName"] = DEVICE_NAME;

  IPAddress ip = WiFi.localIP();
  if (WiFi.status() == WL_CONNECTED && ip != INADDR_NONE) {
    char ipStr[32];
    snprintf(ipStr, sizeof(ipStr), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    jd["ip"] = ipStr;
  } else {
    jd["ip"] = nullptr;
  }

  jd["status"] = "ACTIVE";
  jd["roomId"] = 101;
  jd["notes"] = "UNO R4 + MFRC522";

  JsonObject comp = jd["components"].to<JsonObject>();
  comp["power"] = "USB";
  comp["antenna"] = "MFRC522";
  comp["firmware"] = "r4-1.0.0";

  serializeJson(jd, Serial);
  Serial.println();
}
void normalBeep() {
  digitalWrite(BUZZER_PIN, LOW); delay(150);
  digitalWrite(BUZZER_PIN, HIGH); delay(150);
}
void masterBeep() {
  for (int i = 0; i < 3; i++) {
    digitalWrite(BUZZER_PIN, LOW); delay(150);
    digitalWrite(BUZZER_PIN, HIGH); delay(150);
  }
}
void errorBeep() {
  digitalWrite(BUZZER_PIN, LOW); delay(1000);
  digitalWrite(BUZZER_PIN, HIGH);
}