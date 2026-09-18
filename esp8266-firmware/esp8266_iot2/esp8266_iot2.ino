/*
  ESP-01 (ESP8266) -- SUBSCRIBE ke HiveMQ Cloud -- nyalakan LED

  Alur (PUSH, bukan polling lagi):
  1. ESP8266 konek ke WiFi
  2. ESP8266 konek ke broker HiveMQ Cloud (MQTT over TLS, port 8883)
  3. Subscribe ke topic: smarthome/<DEVICE_NAME>/status
  4. Setiap app Android update status -> backend Vercel publish ke topic itu
     -> broker langsung PUSH pesan ke ESP8266 -> LED update seketika
     (tidak ada lagi jeda polling 3 detik)

  Payload yang diterima (dikirim backend), contoh:
  {"device":"ESP01_01","status":"ON","status2":"OFF",...,"suhu":"0",...}
  Firmware ini cuma pakai field "status" (relay 1), karena ESP-01 cuma
  punya 1 pin output yang bisa dipakai (GPIO0).

  Library yang dibutuhkan (install lewat Library Manager di Arduino IDE):
  - PubSubClient (by Nick O'Leary)
  - ArduinoJson (by Benoit Blanchon)
  - ESP8266WiFi, WiFiClientSecure (bawaan board package ESP8266)
*/

#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

// ------------------- GANTI SESUAI KEBUTUHANMU -------------------
const char* WIFI_SSID     = "IOT";
const char* WIFI_PASSWORD = "rayyanazka";

// Detail broker HiveMQ Cloud kamu (dari tab Overview di HiveMQ Console)
const char* MQTT_HOST = "99a913d804834091bc755acbd559d13f.s1.eu.hivemq.cloud";
const int   MQTT_PORT = 8883;
const char* MQTT_USER = "dede_smarthome";
const char* MQTT_PASS = "rayyanazka";

// Nama device ini -- HARUS SAMA dengan yang dipakai di app Android & backend
const char* DEVICE_NAME = "ESP01_01";
// ------------------------------------------------------------------

const int led = 0;  // LED bawaan ESP-01 (GPIO0, aktif LOW)

WiFiClientSecure secureClient;
PubSubClient mqttClient(secureClient);

char mqttTopic[64];

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
  while (!mqttClient.connected()) {
    Serial.print("Menghubungkan ke HiveMQ...");

    // Client ID harus unik per device yang konek ke broker yang sama
    String clientId = String("esp8266-") + DEVICE_NAME;

    if (mqttClient.connect(clientId.c_str(), MQTT_USER, MQTT_PASS)) {
      Serial.println("Terhubung");
      mqttClient.subscribe(mqttTopic);
      Serial.print("Subscribe ke: ");
      Serial.println(mqttTopic);
    } else {
      Serial.print("Gagal, rc=");
      Serial.print(mqttClient.state());
      Serial.println(" -> coba lagi 2 detik lagi");
      digitalWrite(led, LOW);
      delay(100);
      digitalWrite(led, HIGH);
      delay(1900);
    }
  }
}

void setup() {
  pinMode(led, OUTPUT);
  digitalWrite(led, HIGH); // default LED OFF

  Serial.begin(115200);

  snprintf(mqttTopic, sizeof(mqttTopic), "smarthome/%s/status", DEVICE_NAME);

  connectWiFi();

  // Lewati verifikasi sertifikat -- cara simpel yang umum dipakai di project
  // ESP8266 hobi (root CA store lengkap terlalu berat untuk ESP8266).
  secureClient.setInsecure();

  mqttClient.setServer(MQTT_HOST, MQTT_PORT);
  mqttClient.setCallback(onMqttMessage);

  connectMqtt();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  }

  if (!mqttClient.connected()) {
    connectMqtt();
  }

  mqttClient.loop(); // WAJIB dipanggil terus supaya bisa terima pesan masuk & jaga koneksi
}
