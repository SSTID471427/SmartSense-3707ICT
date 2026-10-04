/*
  SmartSense - Smart Office IoT Automation System
  3707ICT Automation and IoT

  Sensors:
  - DHT22 temperature and humidity
  - LDR ambient light
  - PIR occupancy

  Actuators:
  - LED lighting
  - Servo ventilation simulation
  - Buzzer warning

  Connectivity:
  - Wi-Fi
  - MQTT via the public HiveMQ broker
  - ThingSpeak cloud upload using HTTPS

  Before running:
  1. Replace REPLACE_WITH_YOUR_WRITE_API_KEY with your ThingSpeak Write API Key
     in your private working copy.
  2. Keep the ThingSpeak channel private and do not commit the real API key.
*/

#include <WiFi.h>
#include <PubSubClient.h>
#include <DHT.h>
#include <ESP32Servo.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

// -------------------- Pins --------------------
const int DHT_PIN = 15;      // DHT22 data pin
const int LDR_PIN = 34;      // LDR analogue input
const int PIR_PIN = 26;      // PIR digital input
const int LED_PIN = 13;      // LED lighting output
const int SERVO_PIN = 14;    // Servo PWM output
const int BUZZER_PIN = 27;   // Buzzer output

#define DHTTYPE DHT22
DHT dht(DHT_PIN, DHTTYPE);
Servo ventServo;

// -------------------- Wi-Fi --------------------
const char* WIFI_SSID = "Wokwi-GUEST";
const char* WIFI_PASSWORD = "";

// -------------------- MQTT --------------------
const char* MQTT_SERVER = "broker.hivemq.com";
const int MQTT_PORT = 1883;

// Use a project-specific prefix to reduce collisions on the public broker.
const char* TOPIC_TEMP       = "smartsense/3707ict-team01/temperature";
const char* TOPIC_HUMIDITY   = "smartsense/3707ict-team01/humidity";
const char* TOPIC_LIGHT      = "smartsense/3707ict-team01/light";
const char* TOPIC_OCCUPANCY  = "smartsense/3707ict-team01/occupancy";
const char* TOPIC_LED        = "smartsense/3707ict-team01/light_state";
const char* TOPIC_VENT       = "smartsense/3707ict-team01/vent_state";
const char* TOPIC_ALARM      = "smartsense/3707ict-team01/alarm_state";
const char* TOPIC_OFFICE     = "smartsense/3707ict-team01/office_state";

WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);

// -------------------- ThingSpeak --------------------
const char* THINGSPEAK_WRITE_API_KEY = "REPLACE_WITH_YOUR_WRITE_API_KEY";
WiFiClientSecure cloudClient;

// -------------------- Automation thresholds --------------------
const int DARK_THRESHOLD = 1500;       // Higher Wokwi readings indicate darker conditions
const float VENT_ON_TEMP = 28.0;       // open vent above this
const float VENT_OFF_TEMP = 26.0;      // close vent below this (hysteresis)
const float ALARM_TEMP = 35.0;         // buzzer warning
const float HIGH_HUMIDITY = 70.0;      // used in office-state classification

// Short timeout chosen for a clear demonstration.
// It can be increased for a real office.
const unsigned long OCCUPANCY_TIMEOUT_MS = 10000;

// Sensor sampling interval.
const unsigned long SENSOR_INTERVAL_MS = 2000;
const unsigned long CLOUD_INTERVAL_MS = 20000;

// -------------------- State --------------------
unsigned long lastSensorRead = 0;
unsigned long lastCloudUpload = 0;
unsigned long lastMotionTime = 0;

float temperature = NAN;
float humidity = NAN;
int lightReading = 0;
bool occupied = false;
bool lightOn = false;
bool ventOpen = false;
bool alarmOn = false;
String officeState = "STARTING";

// -------------------- Helpers --------------------
void connectWiFi() {
  Serial.print("Connecting to Wi-Fi");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD, 6);

  while (WiFi.status() != WL_CONNECTED) {
    delay(250);
    Serial.print(".");
  }

  Serial.println();
  Serial.println("Wi-Fi connected");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
}

void connectMQTT() {
  while (!mqttClient.connected()) {
    String clientId = "SmartSense-";
    clientId += String((uint32_t)ESP.getEfuseMac(), HEX);

    Serial.print("Connecting to MQTT...");
    if (mqttClient.connect(clientId.c_str())) {
      Serial.println("connected");
    } else {
      Serial.print("failed, state=");
      Serial.println(mqttClient.state());
      delay(1000);
    }
  }
}

void publishFloat(const char* topic, float value) {
  char buffer[16];
  dtostrf(value, 0, 2, buffer);
  mqttClient.publish(topic, buffer);
}

void publishInt(const char* topic, int value) {
  char buffer[16];
  snprintf(buffer, sizeof(buffer), "%d", value);
  mqttClient.publish(topic, buffer);
}

void publishText(const char* topic, const String& value) {
  mqttClient.publish(topic, value.c_str());
}

String classifyOfficeState() {
  // Rule-based intelligent decision: combines several sensor inputs locally.
  if (!occupied) {
    return "ENERGY_SAVING";
  }

  if (!isnan(temperature) && !isnan(humidity) &&
      temperature > VENT_ON_TEMP && humidity > HIGH_HUMIDITY) {
    return "HOT_HUMID";
  }

  if (lightReading > DARK_THRESHOLD) {
    return "LOW_LIGHT";
  }

  return "COMFORTABLE";
}

void applyAutomation() {
  // Rule 1 + Rule 4: occupancy-aware smart lighting / energy saving.
  lightOn = occupied && (lightReading > DARK_THRESHOLD);
  digitalWrite(LED_PIN, lightOn ? HIGH : LOW);

  // Rule 2: temperature control with hysteresis.
  // Invalid DHT22 readings are never used for control.
  if (!isnan(temperature)) {
    if (!ventOpen && temperature > VENT_ON_TEMP) {
      ventOpen = true;
    } else if (ventOpen && temperature < VENT_OFF_TEMP) {
      ventOpen = false;
    }
  }

  ventServo.write(ventOpen ? 90 : 0);

  // Rule 3: environmental warning.
  alarmOn = (!isnan(temperature) && temperature > ALARM_TEMP);
  digitalWrite(BUZZER_PIN, alarmOn ? HIGH : LOW);

  // Edge intelligent classification.
  officeState = classifyOfficeState();
}

void readSensors() {
  float newTemp = dht.readTemperature();
  float newHumidity = dht.readHumidity();

  // Sensor validation: only replace stored values with valid readings.
  if (!isnan(newTemp) && !isnan(newHumidity)) {
    temperature = newTemp;
    humidity = newHumidity;
  } else {
    Serial.println("DHT22 invalid reading - previous valid values retained");
  }

  lightReading = analogRead(LDR_PIN);

  int motion = digitalRead(PIR_PIN);
  if (motion == HIGH) {
    occupied = true;
    lastMotionTime = millis();
  } else if (occupied && millis() - lastMotionTime > OCCUPANCY_TIMEOUT_MS) {
    occupied = false;
  }

  applyAutomation();
}

void publishMQTTStatus() {
  if (!mqttClient.connected()) {
    connectMQTT();
  }

  if (!isnan(temperature)) publishFloat(TOPIC_TEMP, temperature);
  if (!isnan(humidity)) publishFloat(TOPIC_HUMIDITY, humidity);
  publishInt(TOPIC_LIGHT, lightReading);
  publishText(TOPIC_OCCUPANCY, occupied ? "OCCUPIED" : "EMPTY");
  publishText(TOPIC_LED, lightOn ? "ON" : "OFF");
  publishText(TOPIC_VENT, ventOpen ? "OPEN" : "CLOSED");
  publishText(TOPIC_ALARM, alarmOn ? "ALARM" : "NORMAL");
  publishText(TOPIC_OFFICE, officeState);
}

String buildThingSpeakUrl() {
  String url = "https://api.thingspeak.com/update?api_key=";
  url += THINGSPEAK_WRITE_API_KEY;
  url += "&field1=";
  url += String(temperature, 2);
  url += "&field2=";
  url += String(humidity, 2);
  return url;
}

void uploadToThingSpeak() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("ThingSpeak skipped - Wi-Fi disconnected");
    return;
  }

  if (isnan(temperature) || isnan(humidity)) {
    Serial.println("ThingSpeak skipped - no valid DHT22 reading");
    return;
  }

  if (String(THINGSPEAK_WRITE_API_KEY).startsWith("REPLACE_")) {
    Serial.println("ThingSpeak skipped - add your Write API Key");
    return;
  }

  HTTPClient http;
  String url = buildThingSpeakUrl();

  // Send the update using the ThingSpeak HTTPS REST API.
  cloudClient.setInsecure();
  http.begin(cloudClient, url);
  http.setConnectTimeout(5000);
  http.setTimeout(5000);

  int httpCode = http.GET();
  String response = http.getString();

  Serial.print("ThingSpeak HTTP code: ");
  Serial.print(httpCode);
  Serial.print(" | Entry ID: ");
  Serial.println(response);

  http.end();
}

void printStatus() {
  Serial.println("----- SmartSense -----");
  Serial.print("Temperature: ");
  Serial.print(temperature);
  Serial.println(" C");

  Serial.print("Humidity: ");
  Serial.print(humidity);
  Serial.println(" %");

  Serial.print("LDR: ");
  Serial.println(lightReading);

  Serial.print("Occupancy: ");
  Serial.println(occupied ? "OCCUPIED" : "EMPTY");

  Serial.print("LED: ");
  Serial.println(lightOn ? "ON" : "OFF");

  Serial.print("Vent: ");
  Serial.println(ventOpen ? "OPEN" : "CLOSED");

  Serial.print("Alarm: ");
  Serial.println(alarmOn ? "ALARM" : "NORMAL");

  Serial.print("Office State: ");
  Serial.println(officeState);
  Serial.println();
}

void setup() {
  Serial.begin(115200);

  pinMode(LDR_PIN, INPUT);
  pinMode(PIR_PIN, INPUT);
  pinMode(LED_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);

  digitalWrite(LED_PIN, LOW);
  digitalWrite(BUZZER_PIN, LOW);

  dht.begin();

  ventServo.attach(SERVO_PIN, 500, 2400);
  ventServo.write(0);

  connectWiFi();

  mqttClient.setServer(MQTT_SERVER, MQTT_PORT);

  readSensors();
  publishMQTTStatus();
  printStatus();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  }

  if (!mqttClient.connected()) {
    connectMQTT();
  }
  mqttClient.loop();

  unsigned long now = millis();

  if (now - lastSensorRead >= SENSOR_INTERVAL_MS) {
    lastSensorRead = now;
    readSensors();
    publishMQTTStatus();
    printStatus();
  }

  if (now - lastCloudUpload >= CLOUD_INTERVAL_MS) {
    lastCloudUpload = now;
    uploadToThingSpeak();
  }
}
