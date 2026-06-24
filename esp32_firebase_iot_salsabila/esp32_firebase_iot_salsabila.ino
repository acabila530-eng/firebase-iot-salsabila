#include <Arduino.h>
#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include "addons/TokenHelper.h"
#include "addons/RTDBHelper.h"
#include <DHT.h>

// ===================== KONFIGURASI WiFi =====================
#define WIFI_SSID     "realme 11"     // <-- ganti sesuai WiFi kamu
#define WIFI_PASSWORD "vivaldi11" // <-- ganti sesuai WiFi kamu

// ===================== KONFIGURASI FIREBASE =====================
#define API_KEY      "AIzaSyBeTZoLMCn4Xg7kHnpCda57Y02GVtKaBSQ"
#define DATABASE_URL "https://salsabila-firebase-iot-default-rtdb.asia-southeast1.firebasedatabase.app"

// ===================== PIN DEFINITION =====================
#define DHTPIN   4
#define DHTTYPE  DHT22

#define RELAY1_PIN 23
#define RELAY2_PIN 19
#define RELAY3_PIN 18
#define RELAY4_PIN 5

// ===================== INTERVAL =====================
#define INTERVAL_SENSOR  3000  // kirim data sensor tiap 3 detik
#define INTERVAL_VAR1    400   // jeda antar step variasi 1 (ms)
#define INTERVAL_VAR2    500   // jeda blink variasi 2 (ms)

// ===================== INISIALISASI OBJEK =====================
DHT dht(DHTPIN, DHTTYPE);

// fbdo_stream  : khusus stream relay + mode (realtime, push)
// fbdo         : untuk kirim sensor & operasi lain
FirebaseData fbdo_stream;
FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

// ===================== VARIABEL GLOBAL =====================
unsigned long lastSensorTime = 0;
unsigned long lastAnimTime   = 0;
bool signupOK    = false;
bool streamReady = false;

int  currentMode = 0;
bool relayState[4]  = {false, false, false, false};
int  relayPins[4]   = {RELAY1_PIN, RELAY2_PIN, RELAY3_PIN, RELAY4_PIN};

int  var1Step       = 0;
bool var2BlinkState = false;

volatile bool streamDataAvailable = false;

// ===================== FUNGSI RELAY =====================
void initRelay() {
  for (int i = 0; i < 4; i++) {
    pinMode(relayPins[i], OUTPUT);
    digitalWrite(relayPins[i], HIGH); // aktif LOW: HIGH = OFF saat boot
  }
}

void setRelay(int index, bool state) {
  digitalWrite(relayPins[index], state ? LOW : HIGH);
  relayState[index] = state;
}

void allRelayOff() {
  for (int i = 0; i < 4; i++) setRelay(i, false);
}

void allRelayOn() {
  for (int i = 0; i < 4; i++) setRelay(i, true);
}

// ===================== STREAM CALLBACK =====================
void streamCallback(FirebaseStream data) {
  String path = data.dataPath();
  String type = data.dataType();

  Serial.printf("[Stream] Path: %s | Type: %s\n", path.c_str(), type.c_str());

  // --- Perubahan MODE ---
  if (path == "/mode") {
    int modeBaru = data.intData();
    if (modeBaru != currentMode) {
      Serial.printf("[Mode] %d -> %d\n", currentMode, modeBaru);
      var1Step       = 0;
      var2BlinkState = false;
      allRelayOff();
      currentMode = modeBaru;
    }
    return;
  }

  // --- Perubahan RELAY (path: /relay1, /relay2, /relay3, /relay4) ---
  if (path.startsWith("/relay")) {
    if (currentMode != 0) return;

    int relayIndex = path.substring(6).toInt() - 1;
    if (relayIndex < 0 || relayIndex > 3) return;

    bool state = data.boolData();
    if (state != relayState[relayIndex]) {
      setRelay(relayIndex, state);
      Serial.printf("[Relay %d] -> %s\n", relayIndex + 1, state ? "ON" : "OFF");
    }
    return;
  }

  // --- Payload awal saat stream pertama kali terhubung (tipe JSON) ---
  if (path == "/" && type == "json") {
    FirebaseJson json;
    json.setJsonData(data.jsonString());

    FirebaseJsonData result;
    if (json.get(result, "mode")) {
      currentMode = result.intValue;
      Serial.printf("[Stream Init] Mode: %d\n", currentMode);
    }

    for (int i = 1; i <= 4; i++) {
      String key = "relay" + String(i);
      if (json.get(result, key)) {
        bool state = result.boolValue;
        setRelay(i - 1, state);
        Serial.printf("[Stream Init] Relay %d: %s\n", i, state ? "ON" : "OFF");
      }
    }
  }
}

void streamTimeoutCallback(bool timeout) {
  if (timeout) {
    Serial.println("[Stream] Timeout, reconnecting...");
  }
}

// ===================== MULAI STREAM =====================
void startStream() {
  if (!Firebase.RTDB.beginStream(&fbdo_stream, "/controls")) {
    Serial.printf("[Stream] Gagal mulai: %s\n", fbdo_stream.errorReason().c_str());
    return;
  }
  Firebase.RTDB.setStreamCallback(&fbdo_stream, streamCallback, streamTimeoutCallback);
  streamReady = true;
  Serial.println("[Stream] Aktif pada /controls");
}

// ===================== SETUP =====================
void setup() {
  Serial.begin(115200);
  initRelay();
  dht.begin();

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Menghubungkan ke WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delay(300);
  }
  Serial.println();
  Serial.print("WiFi terhubung. IP: ");
  Serial.println(WiFi.localIP());

  config.api_key      = API_KEY;
  config.database_url = DATABASE_URL;

  if (Firebase.signUp(&config, &auth, "", "")) {
    Serial.println("Firebase sign-up OK");
    signupOK = true;
  } else {
    Serial.printf("Firebase sign-up gagal: %s\n", config.signer.signupError.message.c_str());
  }

  config.token_status_callback = tokenStatusCallback;
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  // Inisialisasi node Firebase jika belum ada
  if (Firebase.ready()) {
    for (int i = 1; i <= 4; i++) {
      String path = "/controls/relay" + String(i);
      Firebase.RTDB.setBool(&fbdo, path.c_str(), false);
    }
    Firebase.RTDB.setInt(&fbdo, "/controls/mode", 0);
  }

  startStream();
}

// ===================== KIRIM DATA SENSOR =====================
void kirimDataSensor() {
  float suhu       = dht.readTemperature();
  float kelembapan = dht.readHumidity();

  if (isnan(suhu) || isnan(kelembapan)) {
    Serial.println("[DHT22] Gagal membaca sensor!");
    return;
  }

  Serial.printf("[DHT22] Suhu: %.1f C | Kelembapan: %.1f%%\n", suhu, kelembapan);

  // Sesuai struktur DB yang disepakati: /sensor/suhu dan /sensor/kelembaban
  Firebase.RTDB.setFloat(&fbdo, "/sensor/suhu", suhu);
  Firebase.RTDB.setFloat(&fbdo, "/sensor/kelembaban", kelembapan); // tanpa lastUpdate
}

// ===================== ANIMASI VARIASI 1 =====================
// Relay menyala satu per satu bergiliran (running light)
void animasiVariasi1() {
  unsigned long now = millis();
  if (now - lastAnimTime < INTERVAL_VAR1) return;
  lastAnimTime = now;

  allRelayOff();

  if (var1Step < 4) {
    setRelay(var1Step, true);
    Serial.printf("[Var1] Relay %d ON\n", var1Step + 1);
    var1Step++;
  } else {
    var1Step = 0;
    Serial.println("[Var1] Reset");
  }
}

// ===================== ANIMASI VARIASI 2 =====================
// Semua relay blink bersamaan
void animasiVariasi2() {
  unsigned long now = millis();
  if (now - lastAnimTime < INTERVAL_VAR2) return;
  lastAnimTime = now;

  if (var2BlinkState) {
    allRelayOn();
    Serial.println("[Var2] Semua ON");
  } else {
    allRelayOff();
    Serial.println("[Var2] Semua OFF");
  }

  var2BlinkState = !var2BlinkState;
}

// ===================== LOOP UTAMA =====================
void loop() {
  if (!Firebase.ready() || !signupOK) {
    Serial.println("[DEBUG] Firebase belum ready!");
    delay(1000);
    return;
  }

  if (Firebase.RTDB.readStream(&fbdo_stream)) {
    if (fbdo_stream.streamAvailable()) {
      Serial.println("[DEBUG] Stream data masuk!");
    }
  } else {
    Serial.printf("[DEBUG] Stream error: %s\n", fbdo_stream.errorReason().c_str());
  }

  unsigned long now = millis();

  if (now - lastSensorTime >= INTERVAL_SENSOR) {
    lastSensorTime = now;
    kirimDataSensor();
  }

  switch (currentMode) {
    case 1:
      animasiVariasi1();
      break;
    case 2:
      animasiVariasi2();
      break;
    default:
      break;
  }
}