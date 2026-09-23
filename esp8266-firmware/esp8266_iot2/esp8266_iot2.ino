/*
  ESP-01 (ESP8266) -- SUBSCRIBE ke HiveMQ Cloud (kontrol relay) + HEARTBEAT ke Vercel (status online)

  Dua jalur berjalan bersamaan:
  1. MQTT (HiveMQ Cloud) -- subscribe ke smarthome/<DEVICE_NAME>/status, buat kontrol
     relay real-time (push instan begitu app Android update lewat backend).
  2. HTTPS (Vercel API) -- kirim "heartbeat" POST /api/heartbeat tiap 15 detik, isinya
     {"device":"ESP01_01","suhu":"..","suhu2":"..","suhu3":".."} -- suhu di sini DUMMY
     (angka acak), karena ESP-01 sudah tidak punya pin sensor tersisa. Endpoint ini
     TERPISAH dari /api/data (kontrol relay), supaya update relay dari app Android
     tidak ikut dianggap sebagai heartbeat device.
     Backend yang menghitung sendiri: kalau lebih dari 35 detik tanpa heartbeat,
     otomatis dianggap OFFLINE (field "status_device" di response API).
     App Android baca status ini lewat REST API biasa, SAMA seperti suhu & relay --
     tidak perlu connect ke broker MQTT sama sekali.

  Library yang dibutuhkan (install lewat Library Manager di Arduino IDE):
  - PubSubClient (by Nick O'Leary)
  - ArduinoJson (by Benoit Blanchon)
  - ESP8266WiFi, WiFiClientSecure, ESP8266HTTPClient (bawaan board package ESP8266)
*/

#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
#define MQTT_KEEPALIVE 60 // default 15 detik kurang toleran terhadap heartbeat HTTPS yang kadang lambat
#include <PubSubClient.h>
#include <ArduinoJson.h>

// ------------------- GANTI SESUAI KEBUTUHANMU -------------------
const char* WIFI_SSID     = "IOT";
const char* WIFI_PASSWORD = "rayyanazka";

// Backend Vercel (dipakai buat heartbeat status online)
const char* HEARTBEAT_URL = "https://backend-esp-8266.vercel.app/api/heartbeat";

// Broker HiveMQ Cloud (dipakai buat kontrol relay real-time)
const char* MQTT_HOST = "99a913d804834091bc755acbd559d13f.s1.eu.hivemq.cloud";
const int   MQTT_PORT = 8883;
const char* MQTT_USER = "dede_smarthome";
const char* MQTT_PASS = "rayyanazka";

// Nama device ini -- HARUS SAMA dengan yang dipakai di app Android & backend
const char* DEVICE_NAME = "ESP01_01";

// Interval heartbeat (ms). Backend anggap OFFLINE kalau lebih dari 35 detik
// tanpa heartbeat, jadi jangan naikkan ini terlalu tinggi (mis. di atas 30 detik).
const unsigned long HEARTBEAT_INTERVAL_MS = 15000;
// ------------------------------------------------------------------

const int led = 0;  // LED bawaan ESP-01 (GPIO0, aktif LOW)

WiFiClientSecure secureMqttClient;
PubSubClient mqttClient(secureMqttClient);

char mqttTopic[64];
unsigned long lastHeartbeat = 0;

void connectWiFi() {
  Serial.print("Menghubungkan ke WiFi: ");
  Serial.println(WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    digitalWrite(led, LOW);
    delay(150);
    digitalWrite(led, HIGH);
    delay(150);
  }

  Serial.println();
  Serial.print("WiFi terhubung. IP: ");
  Serial.println(WiFi.localIP());
}

// Dipanggil otomatis setiap ada pesan baru masuk di topic yang di-subscribe
void onMqttMessage(char* topic, byte* payload, unsigned int length) {
  String message;
  for (unsigned int i = 0; i < length; i++) {
    message += (char)payload[i];
  }

  Serial.print("Pesan masuk [");
  Serial.print(topic);
  Serial.print("]: ");
  Serial.println(message);

  StaticJsonDocument<512> doc;
  DeserializationError err = deserializeJson(doc, message);
  if (err) {
    Serial.println("Gagal parse JSON.");
    return;
  }

  const char* status = doc["status"];
  bool shouldBeOn = status && strcmp(status, "ON") == 0;
  digitalWrite(led, shouldBeOn ? LOW : HIGH); // LED ESP-01 aktif LOW

  Serial.print("LED -> ");
  Serial.println(shouldBeOn ? "ON" : "OFF");
}

void connectMqtt() {
  int failCount = 0;

  while (!mqttClient.connected()) {
    Serial.print("Menghubungkan ke HiveMQ...");

    String clientId = String("esp8266-") + DEVICE_NAME;

    if (mqttClient.connect(clientId.c_str(), MQTT_USER, MQTT_PASS)) {
      Serial.println("Terhubung");
      mqttClient.subscribe(mqttTopic);
      Serial.print("Subscribe ke: ");
      Serial.println(mqttTopic);
      failCount = 0;
    } else {
      failCount++;
      Serial.print("Gagal, rc=");
      Serial.print(mqttClient.state());
      Serial.println(" -> coba lagi 2 detik lagi");
      digitalWrite(led, LOW);
      delay(100);
      digitalWrite(led, HIGH);
      delay(1900);

      // WiFi.status() kadang telat/salah lapor "masih connected" walau WiFi beneran putus
      // (terutama kalau router-nya yang mati, bukan ESP-nya). Kalau MQTT gagal terus,
      // anggap WiFi memang putus, paksa reconnect (ini yang bikin LED kedip lagi).
      if (failCount >= 3) {
        Serial.println("Gagal terus -- paksa WiFi reconnect...");
        WiFi.disconnect();
        delay(200);
        connectWiFi(); // blocking, LED kedip sampai WiFi beneran nyambung lagi
        failCount = 0;
      }
    }
  }
}

// Kirim heartbeat ke backend Vercel -- kasih tahu "saya masih hidup", SEKALIAN
// kirim data suhu (dummy/simulasi, karena ESP-01 sudah tidak punya pin sensor lagi).
// Field relay (status..status5) TIDAK ikut dikirim -- backend otomatis mempertahankan
// nilai lama untuk itu, cuma suhu & last_heartbeat yang ter-refresh.
void sendHeartbeat() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi tidak terhubung, skip heartbeat.");
    return;
  }

  Serial.print("Free heap sebelum heartbeat: ");
  Serial.println(ESP.getFreeHeap());

  WiFiClientSecure client;
  client.setInsecure();
  client.setBufferSizes(512, 512); // hemat memori, penting untuk ESP8266

  HTTPClient http;
  http.begin(client, HEARTBEAT_URL);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(10000); // dilonggarkan -- MQTT sekarang toleran (keepalive 60s), jadi aman dikasih waktu lebih

  // Suhu + kelembaban dummy, digabung jadi satu teks (bukan kolom terpisah)
  float suhu1 = random(250, 350) / 10.0;
  float suhu2 = random(250, 350) / 10.0;
  float suhu3 = random(250, 350) / 10.0;
  int lembab1 = random(40, 90);
  int lembab2 = random(40, 90);
  int lembab3 = random(40, 90);

  String suhuText1 = String(suhu1, 1) + "C, " + String(lembab1) + "%RH";
  String suhuText2 = String(suhu2, 1) + "C, " + String(lembab2) + "%RH";
  String suhuText3 = String(suhu3, 1) + "C, " + String(lembab3) + "%RH";

  // Status cuaca dummy, acak salah satu
  const char* cuacaOptions[] = { "Cerah", "Hujan", "Mendung" };
  const char* cuaca = cuacaOptions[random(0, 3)];

  StaticJsonDocument<256> doc;
  doc["device"] = DEVICE_NAME;
  doc["suhu"] = suhuText1;
  doc["suhu2"] = suhuText2;
  doc["suhu3"] = suhuText3;
  doc["cuaca"] = cuaca;

  String payload;
  serializeJson(doc, payload);

  int httpCode = http.POST(payload);
  if (httpCode > 0) {
    Serial.print("Heartbeat terkirim, kode: ");
    Serial.print(httpCode);
    Serial.print(" | ");
    Serial.println(payload);
  } else {
    Serial.print("Heartbeat gagal: ");
    Serial.println(http.errorToString(httpCode));
  }

  http.end();
  client.stop();
  delay(200); // kasih waktu stack WiFi/TLS "napas" sebelum lanjut proses MQTT
}

void setup() {
  pinMode(led, OUTPUT);
  digitalWrite(led, HIGH); // default LED OFF

  Serial.begin(115200);
  randomSeed(micros()); // biar nilai suhu dummy tidak sama persis tiap boot

  snprintf(mqttTopic, sizeof(mqttTopic), "smarthome/%s/status", DEVICE_NAME);

  connectWiFi();

  secureMqttClient.setInsecure();
  secureMqttClient.setBufferSizes(512, 512); // hemat memori -- penting karena koneksi ini nyala terus
  mqttClient.setServer(MQTT_HOST, MQTT_PORT);
  mqttClient.setCallback(onMqttMessage);
  mqttClient.setBufferSize(512); // payload sekarang lebih besar (ikut suhu, last_heartbeat, dll)

  connectMqtt();
  sendHeartbeat();
  lastHeartbeat = millis();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  }

  if (!mqttClient.connected()) {
    connectMqtt();
  }
  mqttClient.loop();

  if (millis() - lastHeartbeat >= HEARTBEAT_INTERVAL_MS) {
    lastHeartbeat = millis();
    sendHeartbeat();
    mqttClient.loop(); // langsung proses MQTT lagi setelah heartbeat selesai
  }
}
