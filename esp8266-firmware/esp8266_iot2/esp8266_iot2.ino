/*
  ESP-01 (ESP8266) -> HTTPS -> Vercel API -> Neon PostgreSQL (tabel iot2)

  Alur:
  1. ESP8266 konek ke WiFi
  2. Baca tombol di GPIO2 (D2), pakai INPUT_PULLUP (default HIGH, LOW saat ditekan)
  3. LED bawaan ESP-01 (GPIO0, aktif LOW) menyala mengikuti status tombol
  4. Setiap ada PERUBAHAN status tombol, kirim POST ke Vercel:
     https://<url-vercel-kamu>/api/data
     Body JSON: {"device":"ESP01_01","status":"ON"}
  5. Backend simpan ke tabel iot2, kolom "time" diisi otomatis oleh server

  Library yang dibutuhkan (install lewat Library Manager di Arduino IDE):
  - ArduinoJson (by Benoit Blanchon)
  - ESP8266WiFi, ESP8266HTTPClient, WiFiClientSecure
    (sudah bawaan kalau board package "ESP8266" sudah di-install di Boards Manager)
*/

#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

// ------------------- GANTI SESUAI KEBUTUHANMU -------------------
const char* WIFI_SSID     = "ESP-01_AP";
const char* WIFI_PASSWORD = "123456789";

// URL backend di Vercel (setelah nanti kamu deploy), diakhiri /api/data
const char* API_URL = "https://esp8266-iot2-backend.vercel.app/api/data";

// Nama device ini (bebas, asal unik per alat)
const char* DEVICE_NAME = "ESP01_01";
// ------------------------------------------------------------------

// ---- Pin sesuai hardware ESP-01 kamu ----
const int buttonPin = 2;   // Tombol di pin D2 (GPIO2)
const int led = 0;         // LED bawaan ESP-01 (GPIO0, aktif LOW)

int lastState = HIGH;      // default HIGH karena INPUT_PULLUP

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

// ---- Kirim status tombol ke backend Vercel lewat HTTPS POST ----
void publishStatus(const String& buttonState) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi tidak terhubung, skip pengiriman.");
    connectWiFi();
    return;
  }

  WiFiClientSecure client;
  // setInsecure() melewati verifikasi sertifikat HTTPS -- cara simpel yang umum
  // dipakai di project ESP8266 hobi (root CA store penuh terlalu berat untuk ESP8266).
  client.setInsecure();

  HTTPClient http;
  http.begin(client, API_URL);
  http.addHeader("Content-Type", "application/json");

  StaticJsonDocument<200> doc;
  doc["device"] = DEVICE_NAME;
  doc["status"] = buttonState;

  String payload;
  serializeJson(doc, payload);

  Serial.print("Mengirim: ");
  Serial.println(payload);

  int httpCode = http.POST(payload);

  if (httpCode > 0) {
    String response = http.getString();
    Serial.printf("Response code: %d\n", httpCode);
    Serial.println(response);
  } else {
    Serial.printf("Gagal kirim, error: %s\n", http.errorToString(httpCode).c_str());
  }

  http.end();
}

void setup() {
  pinMode(buttonPin, INPUT_PULLUP);
  pinMode(led, OUTPUT);
  digitalWrite(led, HIGH);  // default LED OFF

  Serial.begin(115200);

  connectWiFi();
}

void loop() {
  int state = digitalRead(buttonPin);

  // Kirim hanya kalau ada PERUBAHAN status tombol (bukan polling terus-menerus)
  if (state != lastState) {
    lastState = state;

    if (state == LOW) {
      digitalWrite(led, LOW);   // LED ON (ESP-01 LED aktif LOW)
      Serial.println("Button -> ON");
      publishStatus("ON");
    } else {
      digitalWrite(led, HIGH);  // LED OFF
      Serial.println("Button -> OFF");
      publishStatus("OFF");
    }

    delay(100); // debouncing sederhana
  }
}
