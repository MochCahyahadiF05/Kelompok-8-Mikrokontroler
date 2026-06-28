#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

// ============================================================
// PIN DEFINITION
// ============================================================
#define SENSOR_PIN   35
#define RELAY_PIN    14
#define TRIG_PIN     18   // HC-SR04 Trigger
#define ECHO_PIN     19   // HC-SR04 Echo (gunakan voltage divider 5V->3.3V!)

// ============================================================
// KONFIGURASI WiFi
// ============================================================
const char* WIFI_SSID     = "Molester";
const char* WIFI_PASSWORD = "Catesyi2005";

// ============================================================
// KONFIGURASI MQTT
// ============================================================
const char* MQTT_BROKER    = "smart-irrigation-system.cloud.shiftr.io";
const int   MQTT_PORT      = 1883;
const char* MQTT_CLIENT_ID = "esp32-irrigation";
const char* MQTT_USER      = "smart-irrigation-system";
const char* MQTT_PASS      = "2TnGUiGgmfpPnVod";

// Topic MQTT
const char* TOPIC_SENSOR  = "irrigation/sensor";
const char* TOPIC_STATUS  = "irrigation/status";
const char* TOPIC_ALERT   = "irrigation/alert";
const char* TOPIC_CONTROL = "irrigation/control";

// ============================================================
// KONFIGURASI IRIGASI (ubah langsung di sini, tidak lewat web)
// ============================================================
const int           THRESHOLD_DRY    = 2800;  // ADC > ini = kering = pompa nyala (auto)
const int           THRESHOLD_WET    = 2200;  // ADC < ini = basah  = pompa mati  (auto)
const unsigned long MAX_PUMP_ON_MS   = 30000; // Maks pompa nyala 30 detik (proteksi)
const unsigned long READ_INTERVAL_MS = 2000;  // Baca sensor tiap 2 detik
const unsigned long PUB_INTERVAL_MS  = 5000;  // Publish MQTT tiap 5 detik

// ============================================================
// KONFIGURASI TANGKI AIR (HC-SR04)
// ============================================================
const float TANK_HEIGHT_CM      = 50.0; // Tinggi tangki (jarak sensor ke dasar tangki, cm)
const float SENSOR_OFFSET_CM    = 2.0;  // Jarak sensor ke permukaan air saat tangki PENUH (cm)
const float TANK_EMPTY_PERCENT  = 10.0; // % di bawah ini dianggap KOSONG -> pompa dikunci mati
const int   ULTRASONIC_SAMPLES  = 3;    // Jumlah sampel untuk dirata-rata (filter noise)

// ============================================================
// MODE KONTROL POMPA
// ============================================================
// Tiga mode eksklusif:
typedef enum { MODE_AUTO, MODE_MANUAL_ON, MODE_MANUAL_OFF } PumpMode;
PumpMode pumpMode = MODE_AUTO;

// ============================================================
// VARIABEL GLOBAL
// ============================================================
bool          pumpState       = false;
unsigned long pumpStartTime   = 0;
unsigned long lastReadTime    = 0;
unsigned long lastPubTime     = 0;
int           moistureRaw     = 0;
int           moisturePercent = 0;

// Level air tangki
float         waterDistanceCm = -1;    // jarak sensor->permukaan air (cm), -1 = belum/gagal baca
int            waterLevelPercent = 0;  // 0-100% penuh
bool          tankEmpty       = false; // true jika level <= TANK_EMPTY_PERCENT
bool          tankEmptyAlertSent = false;

WiFiClient   espClient;
PubSubClient mqttClient(espClient);

// ============================================================
// FORWARD DECLARATION
// ============================================================
void pumpOn(bool publishNow = true);
void pumpOff(bool publishNow = true);

// ============================================================
// WiFi
// ============================================================
void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  Serial.printf("\n[WiFi] Menghubungkan ke %s", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int attempt = 0;
  while (WiFi.status() != WL_CONNECTED && attempt++ < 20) {
    delay(500); Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n[WiFi] Terhubung! IP: %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("\n[WiFi] GAGAL. Lanjut offline.");
  }
}

// ============================================================
// MQTT Callback
// ============================================================
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg = "";
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
  msg.trim();
  Serial.printf("[MQTT] Terima [%s]: %s\n", topic, msg.c_str());

  if (String(topic) == TOPIC_CONTROL) {
    if (msg == "ON") {
      // Paksa pompa nyala, sensor diabaikan
      pumpMode = MODE_MANUAL_ON;
      pumpOn();
      Serial.println("[MODE] MANUAL ON - sensor diabaikan");

    } else if (msg == "OFF") {
      // Paksa pompa mati, sensor TIDAK bisa nyalakan otomatis
      pumpMode = MODE_MANUAL_OFF;
      pumpOff();
      Serial.println("[MODE] MANUAL OFF - pompa dikunci mati, sensor diabaikan");

    } else if (msg == "AUTO") {
      // Kembalikan ke mode otomatis
      pumpMode = MODE_AUTO;
      Serial.println("[MODE] AUTO - sensor kembali mengendalikan pompa");
      // Langsung evaluasi kondisi sensor sekarang
      if (moistureRaw > THRESHOLD_DRY) {
        pumpOn();
      } else if (moistureRaw < THRESHOLD_WET) {
        pumpOff();
      }
    }
  }
}

// ============================================================
// MQTT Connect
// ============================================================
void connectMQTT() {
  if (WiFi.status() != WL_CONNECTED) return;
  Serial.printf("[MQTT] Menghubungkan ke %s:%d ...\n", MQTT_BROKER, MQTT_PORT);
  int attempt = 0;
  while (!mqttClient.connected() && attempt++ < 3) {
    bool ok = mqttClient.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASS);
    if (ok) {
      Serial.println("[MQTT] Terhubung!");
      mqttClient.subscribe(TOPIC_CONTROL);
      mqttClient.publish(TOPIC_STATUS, "ONLINE");
    } else {
      Serial.printf("[MQTT] Gagal rc=%d, retry...\n", mqttClient.state());
      delay(2000);
    }
  }
}

// ============================================================
// Publish data sensor
// ============================================================
void publishSensorData() {
  if (!mqttClient.connected()) return;

  const char* modeStr;
  if      (pumpMode == MODE_MANUAL_ON)  modeStr = "MANUAL_ON";
  else if (pumpMode == MODE_MANUAL_OFF) modeStr = "MANUAL_OFF";
  else                                   modeStr = "AUTO";

  StaticJsonDocument<320> doc;
  doc["moisture_raw"] = moistureRaw;
  doc["moisture_pct"] = moisturePercent;
  doc["pump_state"]   = pumpState ? "ON" : "OFF";
  doc["mode"]         = modeStr;

  if      (moisturePercent >= 60) doc["soil_status"] = "BASAH";
  else if (moisturePercent >= 35) doc["soil_status"] = "LEMBAB";
  else                             doc["soil_status"] = "KERING";

  doc["pump_on_sec"]      = pumpState ? (millis() - pumpStartTime) / 1000 : 0;

  // Data level air tangki
  doc["water_level_pct"] = waterLevelPercent;
  doc["water_distance_cm"] = waterDistanceCm;
  doc["tank_empty"]       = tankEmpty;

  char payload[320];
  serializeJson(doc, payload);
  mqttClient.publish(TOPIC_SENSOR, payload);
  Serial.printf("[MQTT] Publish: %s\n", payload);
}

// ============================================================
// Sensor
// ============================================================
void readSensor() {
  long sum = 0;
  for (int i = 0; i < 5; i++) { sum += analogRead(SENSOR_PIN); delay(10); }
  moistureRaw     = sum / 5;
  moisturePercent = constrain(map(moistureRaw, 4095, 0, 0, 100), 0, 100);
}

// ============================================================
// Sensor Level Air (HC-SR04)
// ============================================================
// Mengembalikan jarak dalam cm, atau -1 jika gagal/tidak ada pantulan (timeout)
float readUltrasonicCm() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  // Timeout 25ms (~430cm) supaya loop tidak macet jika tidak ada echo
  unsigned long duration = pulseIn(ECHO_PIN, HIGH, 25000UL);
  if (duration == 0) return -1; // timeout / tidak ada pantulan

  float distanceCm = duration * 0.0343f / 2.0f;
  return distanceCm;
}

void readWaterLevel() {
  float sum = 0;
  int   validCount = 0;

  for (int i = 0; i < ULTRASONIC_SAMPLES; i++) {
    float d = readUltrasonicCm();
    if (d > 0) { sum += d; validCount++; }
    delay(30); // jeda antar tembakan agar tidak saling interferensi
  }

  if (validCount == 0) {
    Serial.println("[ULTRASONIC] Gagal baca (timeout) - cek wiring/sensor.");
    return;
  }

  waterDistanceCm = sum / validCount;

  // Jarak efektif dari permukaan air ke dasar tangki (memperhitungkan offset sensor)
  float usableHeight = TANK_HEIGHT_CM - SENSOR_OFFSET_CM;
  float waterHeight   = (TANK_HEIGHT_CM - waterDistanceCm) - SENSOR_OFFSET_CM;
  waterHeight = constrain(waterHeight, 0, usableHeight);

  waterLevelPercent = (usableHeight > 0) ? (int)((waterHeight / usableHeight) * 100.0f) : 0;
  waterLevelPercent = constrain(waterLevelPercent, 0, 100);

  bool wasEmpty = tankEmpty;
  tankEmpty = (waterLevelPercent <= TANK_EMPTY_PERCENT);

  // Reset flag alert begitu air sudah naik lagi di atas ambang kosong
  if (wasEmpty && !tankEmpty) tankEmptyAlertSent = false;
}

// ============================================================
// Pompa
// ============================================================
void pumpOn(bool publishNow) {
  if (pumpState) return;
  if (tankEmpty) {
    Serial.println("[PROTEKSI] Permintaan pompa ON ditolak - tangki air kosong!");
    if (publishNow && mqttClient.connected())
      mqttClient.publish(TOPIC_ALERT, "PUMP_BLOCKED - tangki kosong, pompa tidak dinyalakan");
    return;
  }
  digitalWrite(RELAY_PIN, LOW);
  pumpState     = true;
  pumpStartTime = millis();
  Serial.println("[POMPA] ON");
  if (publishNow && mqttClient.connected()) {
    const char* s = (pumpMode == MODE_MANUAL_ON) ? "ON_MANUAL" : "ON_AUTO";
    mqttClient.publish(TOPIC_STATUS, s);
  }
}

void pumpOff(bool publishNow) {
  if (!pumpState) return;
  digitalWrite(RELAY_PIN, HIGH);
  pumpState = false;
  Serial.println("[POMPA] OFF");
  if (publishNow && mqttClient.connected()) {
    mqttClient.publish(TOPIC_STATUS, "OFF");
  }
}

// ============================================================
// Logika kontrol irigasi
// ============================================================
void controlIrrigation() {
  if (tankEmpty) {
    if (pumpState) {
      pumpOff();
      Serial.println("[PROTEKSI] Tangki air kosong! Pompa dimatikan paksa.");
    }
    if (!tankEmptyAlertSent && mqttClient.connected()) {
      mqttClient.publish(TOPIC_ALERT, "TANK_EMPTY - pompa dikunci mati sampai air terisi kembali");
      tankEmptyAlertSent = true;
    }
    return; // jangan lanjutkan logika mode apapun selama tangki kosong
  }

  // MODE_MANUAL_OFF: pompa dikunci mati, sensor tidak bisa nyalakan
  if (pumpMode == MODE_MANUAL_OFF) {
    if (pumpState) pumpOff();  // pastikan tetap mati
    return;
  }

  // MODE_MANUAL_ON: pompa dikunci nyala, cek timeout saja
  if (pumpMode == MODE_MANUAL_ON) {
    if (!pumpState) pumpOn();
    // Timeout tetap berlaku sebagai safety, tapi mode tidak berubah ke AUTO
    if (millis() - pumpStartTime >= MAX_PUMP_ON_MS) {
      Serial.println("[PERINGATAN] Timeout! Pompa mati paksa (mode tetap MANUAL_ON).");
      pumpOff();
      if (mqttClient.connected())
        mqttClient.publish(TOPIC_ALERT, "PUMP_TIMEOUT - matikan manual dulu via web");
    }
    return;
  }

  // MODE_AUTO: sensor menentukan
  if (!pumpState) {
    if (moistureRaw > THRESHOLD_DRY) pumpOn();
  } else {
    if (moistureRaw < THRESHOLD_WET) {
      Serial.println("[INFO] Tanah sudah basah, pompa mati.");
      pumpOff();
    } else if (millis() - pumpStartTime >= MAX_PUMP_ON_MS) {
      Serial.println("[PERINGATAN] Timeout! Pompa mati paksa.");
      pumpOff();
      if (mqttClient.connected())
        mqttClient.publish(TOPIC_ALERT, "PUMP_TIMEOUT - mati paksa setelah timeout");
    }
  }
}

void printStatus() {
  const char* modeStr;
  if      (pumpMode == MODE_MANUAL_ON)  modeStr = "MANUAL ON (sensor diabaikan)";
  else if (pumpMode == MODE_MANUAL_OFF) modeStr = "MANUAL OFF (sensor diabaikan)";
  else                                   modeStr = "OTOMATIS";

  Serial.println("----------------------------------");
  Serial.printf("Moisture ADC : %d\n", moistureRaw);
  Serial.printf("Moisture %%   : %d%%\n", moisturePercent);
  Serial.printf("Pompa        : %s\n", pumpState ? "ON" : "OFF");
  Serial.printf("Mode         : %s\n", modeStr);
  Serial.printf("MQTT         : %s\n", mqttClient.connected() ? "OK" : "Terputus");
  if (pumpState)
    Serial.printf("Waktu ON     : %lu detik\n", (millis() - pumpStartTime) / 1000);
  if      (moisturePercent >= 60) Serial.println("Tanah        : BASAH");
  else if (moisturePercent >= 35) Serial.println("Tanah        : LEMBAB");
  else                             Serial.println("Tanah        : KERING");

  if (waterDistanceCm > 0)
    Serial.printf("Jarak Air    : %.1f cm\n", waterDistanceCm);
  else
    Serial.println("Jarak Air    : gagal baca (cek wiring)");
  Serial.printf("Level Air    : %d%%%s\n", waterLevelPercent, tankEmpty ? "  [TANGKI KOSONG - POMPA DIKUNCI]" : "");
}

// ============================================================
// SETUP
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, HIGH);  // Pompa MATI saat awal
  analogSetAttenuation(ADC_11db);

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  digitalWrite(TRIG_PIN, LOW);

  Serial.println("==================================");
  Serial.println(" Smart Irrigation System - ESP32  ");
  Serial.println("==================================");
  Serial.printf("Threshold DRY : %d\n", THRESHOLD_DRY);
  Serial.printf("Threshold WET : %d\n", THRESHOLD_WET);
  Serial.printf("Max pompa ON  : %lu ms\n", MAX_PUMP_ON_MS);
  Serial.printf("Tinggi tangki : %.1f cm (ambang kosong: %.0f%%)\n", TANK_HEIGHT_CM, TANK_EMPTY_PERCENT);

  connectWiFi();

  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setKeepAlive(60);
  connectMQTT();
  readWaterLevel();

  Serial.println("----------------------------------");
}

// ============================================================
// LOOP
// ============================================================
void loop() {
  unsigned long now = millis();

  if (WiFi.status() != WL_CONNECTED) connectWiFi();
  if (!mqttClient.connected()) connectMQTT();
  mqttClient.loop();

  if (now - lastReadTime >= READ_INTERVAL_MS) {
    lastReadTime = now;
    readSensor();
    readWaterLevel();
    controlIrrigation();
    printStatus();
  }

  if (now - lastPubTime >= PUB_INTERVAL_MS) {
    lastPubTime = now;
    publishSensorData();
  }
}
