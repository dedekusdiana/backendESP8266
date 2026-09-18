/*
  ESP-01 (ESP8266) -- baca status dari Neon (lewat Vercel API) -- nyalakan LED

  Alur (KEBALIKAN dari versi sebelumnya):
  1. ESP8266 konek ke WiFi (perlu akses internet, karena akses server di Vercel)
  2. Setiap POLL_INTERVAL_MS, ESP8266 GET ke:
     https://<url-vercel-kamu>/api/status/ESP01_01
  3. Kalau field "status" di response == "ON" -> LED menyala
     Kalau "OFF" (atau device belum ada datanya) -> LED mati
  4. ESP8266 TIDAK mengirim data apa pun -- murni jadi "penerima perintah"

  Catatan: polling tiap 3 detik cukup ringan untuk ESP8266. Kalau mau lebih
  responsif bisa diturunkan, tapi makin sering makin berat ke memori ESP8266.

  Library yang dibutuhkan (install lewat Library Manager di Arduino IDE):
  - ArduinoJson (by Benoit Blanchon)
  - ESP8266WiFi, ESP8266HTTPClient, WiFiClientSecure (bawaan board package ESP8266)
*/

#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

// ------------------- GANTI SESUAI KEBUTUHANMU -------------------
const char* WIFI_SSID     = "IOT";
const char* WIFI_PASSWORD = "rayyanazka";

// URL backend Vercel + /api/status/<nama-device>
const char* STATUS_URL = "https://backend-esp-8266.vercel.app/api/status/ESP01_01";

// Interval polling (ms). 3000 = tiap 3 detik.
const unsigned long POLL_INTERVAL_MS = 3000;
// ------------------------------------------------------------------

const int led = 0;  // LED bawaan ESP-01 (GPIO0, aktif LOW)

unsigned long lastPollTime = 0;
bool lastLedOn = false;

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

// Ambil status terbaru dari server, lalu set LED sesuai isinya
void pollStatusAndSetLed() {
  Serial.print("Free heap: ");
  Serial.println(ESP.getFreeHeap());

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi tidak terhubung, coba sambung ulang...");
    connectWiFi();
    return;
  }

  WiFiClientSecure client;
  client.setInsecure(); // lewati verifikasi sertifikat, cara simpel untuk project hobi

  // PENTING untuk ESP8266: buffer TLS default (~16KB) terlalu besar untuk RAM
  // ESP8266 (~50KB total). Kalau tidak diperkecil, setelah beberapa kali request
  // heap jadi terfragmentasi dan handshake HTTPS berikutnya gagal timeout.
  client.setBufferSizes(512, 512);

  HTTPClient http;
  http.begin(client, STATUS_URL);
  http.setTimeout(8000); // Vercel kadang perlu waktu lebih (cold start)

  int httpCode = http.GET();

  if (httpCode == 200) {
    String response = http.getString();
    Serial.print("Response: ");
    Serial.println(response);

    StaticJsonDocument<256> doc;
    DeserializationError err = deserializeJson(doc, response);

    if (!err) {
      const char* status = doc["status"];
      bool shouldBeOn = status && strcmp(status, "ON") == 0;

      Serial.print("status field = \"");
      Serial.print(status ? status : "(null)");
      Serial.print("\" -> shouldBeOn = ");
      Serial.println(shouldBeOn ? "true" : "false");

      // Selalu set ulang pin LED tiap poll (bukan cuma pas berubah),
      // supaya tidak ada risiko LED "nyangkut" di state yang salah.
      lastLedOn = shouldBeOn;
      digitalWrite(led, shouldBeOn ? LOW : HIGH); // LED ESP-01 aktif LOW
    } else {
      Serial.print("Gagal parse JSON response: ");
      Serial.println(err.c_str());
    }
  } else if (httpCode == 404) {
    // Belum ada data sama sekali untuk device ini -> anggap OFF
    lastLedOn = false;
    digitalWrite(led, HIGH);
  } else {
    Serial.printf("GET gagal, kode: %d\n", httpCode);
  }

  http.end();
}

void setup() {
  pinMode(led, OUTPUT);
  digitalWrite(led, HIGH); // default LED OFF

  Serial.begin(115200);

  connectWiFi();
}

void loop() {
  unsigned long now = millis();

  if (now - lastPollTime >= POLL_INTERVAL_MS) {
    lastPollTime = now;
    pollStatusAndSetLed();
  }
}
