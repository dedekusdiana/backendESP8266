/*
  ESP-01 (ESP8266) -- SUBSCRIBE ke HiveMQ Cloud (kontrol relay) + HEARTBEAT ke Vercel (status online)

  Dua jalur berjalan bersamaan:
  1. MQTT (HiveMQ Cloud) -- subscribe ke smarthome/<nama device>/status, buat kontrol
     relay real-time (push instan begitu app Android update lewat backend).
  2. HTTPS (Vercel API) -- kirim "heartbeat" POST /api/heartbeat tiap 15 detik, isinya
     {"device":"ESP-A1B2C3","suhu":"..","suhu2":"..","suhu3":".."} -- suhu di sini DUMMY
     (angka acak), karena ESP-01 sudah tidak punya pin sensor tersisa. Endpoint ini
     TERPISAH dari /api/data (kontrol relay), supaya update relay dari app Android
     tidak ikut dianggap sebagai heartbeat device.
     Backend yang menghitung sendiri: kalau lebih dari 35 detik tanpa heartbeat,
     otomatis dianggap OFFLINE (field "status_device" di response API).
     App Android baca status ini lewat REST API biasa, SAMA seperti suhu & relay --
     tidak perlu connect ke broker MQTT sama sekali.

  3. AUTO (jadwal lampu) -- jadwal nyala/mati (field auto1 & auto2 dari database) diterima
     lewat MQTT (retained, jadi tetap sampai ke ESP walau baru nyala). Jamnya dicek DI ESP
     sendiri pakai jam NTP (WIB), jadi jadwal tetap jalan walau internet/server sedang putus.
     auto1 -> Relay 1 (GPIO0), auto2 -> Relay 2 (GPIO2). Setiap jadwal berganti nyala/mati,
     ESP melapor ke backend (POST /api/heartbeat) supaya status di app ikut berubah.

  4. SETUP WiFi (untuk produk yang dijual) -- SSID/password WiFi TIDAK ditanam di firmware.
     Pertama kali menyala (atau kalau WiFi lama tidak ketemu > 2 menit), ESP membuat WiFi
     sendiri bernama "SmartHome-<ChipID>". Pelanggan sambungkan HP ke WiFi itu, halaman setup
     terbuka otomatis, pilih WiFi rumah + isi password -> tersimpan di flash & ESP restart koneksi.
     Nama device otomatis dari Chip ID ESP ("ESP-<ChipID>"), jadi unik untuk setiap alat.

  Library yang dibutuhkan (install lewat Library Manager di Arduino IDE):
  - WiFiManager (by tzapu)
  - PubSubClient (by Nick O'Leary)
  - ArduinoJson (by Benoit Blanchon)
  - ESP8266WiFi, WiFiClientSecure, ESP8266HTTPClient (bawaan board package ESP8266)
*/

#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
#include <WiFiManager.h>   // by tzapu -- portal setup WiFi
#include <time.h>
#define MQTT_KEEPALIVE 60 // default 15 detik kurang toleran terhadap heartbeat HTTPS yang kadang lambat
#include <PubSubClient.h>
#include <ArduinoJson.h>

// ------------------- GANTI SESUAI KEBUTUHANMU -------------------
// WiFi TIDAK diisi di sini -- diatur pelanggan lewat portal setup (lihat catatan no. 4 di atas).
// Password WiFi setup milik alat (untuk terhubung ke portal). Kosong = terbuka (paling mudah untuk
// pelanggan). Kalau diisi, minimal 8 karakter -- cetak di stiker/kemasan produk.
const char* AP_PASSWORD = "";

// Kalau WiFi tersimpan gagal tersambung selama ini, alat membuka portal setup lagi
// (supaya pelanggan bisa ganti WiFi / password router).
const unsigned long WIFI_RETRY_BEFORE_PORTAL_MS = 120000UL; // 2 menit
const unsigned int  PORTAL_TIMEOUT_S = 180;                 // portal ditutup otomatis setelah 3 menit (kalau sudah ada WiFi tersimpan)

// Set true SEKALI saat testing untuk menghapus WiFi tersimpan (memaksa portal muncul), lalu kembalikan ke false.
const bool RESET_WIFI_ON_BOOT = false;

// LED status opsional (aktif LOW). -1 = tidak ada. JANGAN isi pin relay (0 / 2), nanti relay ikut berkedip.
const int STATUS_LED = -1;

// Backend Vercel (dipakai buat heartbeat status online)
const char* HEARTBEAT_URL = "https://backend-esp-8266.vercel.app/api/heartbeat";

// Broker HiveMQ Cloud (dipakai buat kontrol relay real-time)
const char* MQTT_HOST = "99a913d804834091bc755acbd559d13f.s1.eu.hivemq.cloud";
const int   MQTT_PORT = 8883;
const char* MQTT_USER = "dede_smarthome";
const char* MQTT_PASS = "rayyanazka";

// Interval heartbeat (ms). Backend anggap OFFLINE kalau lebih dari 35 detik
// tanpa heartbeat, jadi jangan naikkan ini terlalu tinggi (mis. di atas 30 detik).
const unsigned long HEARTBEAT_INTERVAL_MS = 15000;
// ------------------------------------------------------------------

// Nama device unik, dibuat otomatis dari Chip ID ESP di setup() -- mis. "ESP-A1B2C3".
// Ini yang muncul di daftar device di app Android.
char deviceName[24];
char apName[32]; // nama WiFi setup, mis. "SmartHome-A1B2C3"

// Relay yang bisa dijadwalkan. Urutan = auto1, auto2.
// Ganti pin / nama field sesuai wiring kamu (aktif LOW: LOW = nyala).
const int  NUM_AUTO = 2;
const int  RELAY_PINS[NUM_AUTO]   = { 0, 2 };                // GPIO0 (Relay 1), GPIO2 (Relay 2)
const char* RELAY_FIELDS[NUM_AUTO] = { "status", "status2" };  // kolom database relay
const char* AUTO_FIELDS[NUM_AUTO]  = { "auto1", "auto2" };     // kolom database jadwal

struct AutoSchedule {
  bool enabled = false;
  int  onMinute = 18 * 60;   // menit sejak 00:00
  int  offMinute = 6 * 60;
  char raw[24] = "";         // teks asli, untuk deteksi perubahan jadwal
  int  lastDesired = -1;     // -1 = belum dievaluasi, 0 = harusnya mati, 1 = harusnya nyala
  bool pendingReport = false; // perlu lapor status relay ke backend
};
AutoSchedule autoSched[NUM_AUTO];
unsigned long lastReportTry = 0;
unsigned long lastAutoCheck = 0;

WiFiClientSecure secureMqttClient;
PubSubClient mqttClient(secureMqttClient);

char mqttTopic[64];
unsigned long lastHeartbeat = 0;

void statusLed(bool on) {
  if (STATUS_LED >= 0) digitalWrite(STATUS_LED, on ? LOW : HIGH);
}

// Buka portal setup WiFi (WiFi milik alat) sampai pelanggan mengisi WiFi rumahnya.
// Kalau belum pernah ada WiFi tersimpan, portal terbuka TANPA batas waktu.
void openConfigPortal() {
  bool hasSaved = WiFi.SSID().length() > 0;

  Serial.println();
  Serial.print("Portal setup WiFi dibuka. Sambungkan HP ke WiFi: ");
  Serial.println(apName);

  WiFiManager wm;
  wm.setConfigPortalTimeout(hasSaved ? PORTAL_TIMEOUT_S : 0);
  wm.setConnectTimeout(20);
  wm.setBreakAfterConfig(true);
  wm.startConfigPortal(apName, strlen(AP_PASSWORD) >= 8 ? AP_PASSWORD : NULL);
}

// Sambung ke WiFi tersimpan. Kalau belum ada / gagal cukup lama -> buka portal setup.
// Blocking sampai benar-benar tersambung (sama seperti perilaku sebelumnya).
void connectWiFi() {
  WiFi.mode(WIFI_STA);

  while (WiFi.status() != WL_CONNECTED) {
    if (WiFi.SSID().length() > 0) {
      Serial.print("Menghubungkan ke WiFi tersimpan: ");
      Serial.println(WiFi.SSID());
      WiFi.begin(); // pakai kredensial yang tersimpan di flash

      unsigned long t0 = millis();
      while (WiFi.status() != WL_CONNECTED && millis() - t0 < WIFI_RETRY_BEFORE_PORTAL_MS) {
        Serial.print(".");
        statusLed(true);
        delay(150);
        statusLed(false);
        delay(350);
      }
      Serial.println();
    }

    if (WiFi.status() != WL_CONNECTED) {
      openConfigPortal(); // belum ada WiFi tersimpan, atau gagal tersambung > 2 menit
    }
  }

  Serial.print("WiFi terhubung. IP: ");
  Serial.println(WiFi.localIP());
}

// Pasang status relay ke pin (aktif LOW)
void setRelay(int idx, bool on) {
  digitalWrite(RELAY_PINS[idx], on ? LOW : HIGH);
}

// Parse "ON,18:00,06:00" -> isi struct jadwal. Return false kalau format salah.
bool parseAuto(const char* text, AutoSchedule& a) {
  char flag[8];
  int h1, m1, h2, m2;
  if (sscanf(text, "%7[A-Za-z],%d:%d,%d:%d", flag, &h1, &m1, &h2, &m2) != 5) return false;
  if (h1 < 0 || h1 > 23 || h2 < 0 || h2 > 23 || m1 < 0 || m1 > 59 || m2 < 0 || m2 > 59) return false;
  a.enabled = (strcasecmp(flag, "ON") == 0);
  a.onMinute = h1 * 60 + m1;
  a.offMinute = h2 * 60 + m2;
  return true;
}

// Apakah menit sekarang masuk jendela nyala? Mendukung jadwal lewat tengah malam (18:00 -> 06:00).
bool inWindow(int nowMin, int onMin, int offMin) {
  if (onMin == offMin) return false;
  if (onMin < offMin) return nowMin >= onMin && nowMin < offMin;
  return nowMin >= onMin || nowMin < offMin;
}

// Cek jadwal tiap ~1 detik. Aksi HANYA dilakukan saat status "seharusnya" berubah
// (atau saat jadwal baru diterima), jadi kontrol manual dari app tetap bisa dipakai
// di antara jam nyala dan jam mati.
void checkAutoSchedules() {
  time_t now = time(nullptr);
  if (now < 1700000000) return; // NTP belum sinkron -> jangan bertindak dulu

  struct tm t;
  localtime_r(&now, &t);
  int nowMin = t.tm_hour * 60 + t.tm_min;

  for (int i = 0; i < NUM_AUTO; i++) {
    AutoSchedule& a = autoSched[i];
    if (!a.enabled) { a.lastDesired = -1; continue; }

    int desired = inWindow(nowMin, a.onMinute, a.offMinute) ? 1 : 0;
    if (desired != a.lastDesired) {
      a.lastDesired = desired;
      setRelay(i, desired == 1);
      a.pendingReport = true;
      Serial.print("AUTO ");
      Serial.print(i + 1);
      Serial.print(" -> ");
      Serial.println(desired ? "ON" : "OFF");
    }
  }
}

// Lapor status relay ke backend (POST /api/heartbeat, field status/status2). Return true kalau sukses.
bool postRelayState(int idx) {
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClientSecure client;
  client.setInsecure();
  client.setBufferSizes(512, 512);

  HTTPClient http;
  http.begin(client, HEARTBEAT_URL); // laporan relay juga lewat /api/heartbeat (tanpa kode aktivasi)
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(10000);

  StaticJsonDocument<128> doc;
  doc["device"] = deviceName;
  doc[RELAY_FIELDS[idx]] = (digitalRead(RELAY_PINS[idx]) == LOW) ? "ON" : "OFF";
  String payload;
  serializeJson(doc, payload);

  int code = http.POST(payload);
  Serial.print("Lapor relay ");
  Serial.print(idx + 1);
  Serial.print(" -> kode ");
  Serial.println(code);

  http.end();
  client.stop();
  delay(200);
  return code >= 200 && code < 300;
}

// Kirim laporan yang tertunda (dicoba ulang tiap 10 detik kalau gagal)
void reportPendingRelays() {
  if (millis() - lastReportTry < 10000) return;
  for (int i = 0; i < NUM_AUTO; i++) {
    if (autoSched[i].pendingReport) {
      lastReportTry = millis();
      if (postRelayState(i)) autoSched[i].pendingReport = false;
      return; // satu laporan per putaran, biar tidak menahan MQTT terlalu lama
    }
  }
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

  // 1) Jadwal auto (kalau berubah, paksa evaluasi ulang supaya langsung berlaku)
  for (int i = 0; i < NUM_AUTO; i++) {
    const char* raw = doc[AUTO_FIELDS[i]];
    if (raw && strcmp(raw, autoSched[i].raw) != 0) {
      AutoSchedule parsed;
      if (parseAuto(raw, parsed)) {
        autoSched[i].enabled = parsed.enabled;
        autoSched[i].onMinute = parsed.onMinute;
        autoSched[i].offMinute = parsed.offMinute;
        autoSched[i].lastDesired = -1;
        strlcpy(autoSched[i].raw, raw, sizeof(autoSched[i].raw));
        Serial.print("Jadwal ");
        Serial.print(AUTO_FIELDS[i]);
        Serial.print(": ");
        Serial.println(raw);
      }
    }
  }

  // 2) Status relay dari app
  for (int i = 0; i < NUM_AUTO; i++) {
    const char* st = doc[RELAY_FIELDS[i]];
    if (!st) continue;
    bool shouldBeOn = strcmp(st, "ON") == 0;
    setRelay(i, shouldBeOn);
    Serial.print("Relay ");
    Serial.print(i + 1);
    Serial.print(" -> ");
    Serial.println(shouldBeOn ? "ON" : "OFF");
  }
}

void connectMqtt() {
  int failCount = 0;

  while (!mqttClient.connected()) {
    Serial.print("Menghubungkan ke HiveMQ...");

    String clientId = String("esp8266-") + deviceName;

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
      statusLed(true);
      delay(100);
      statusLed(false);
      delay(1900);

      // WiFi.status() kadang telat/salah lapor "masih connected" walau WiFi beneran putus
      // (terutama kalau router-nya yang mati, bukan ESP-nya). Kalau MQTT gagal terus,
      // anggap WiFi memang putus, paksa reconnect (ini yang bikin LED kedip lagi).
      if (failCount >= 3) {
        Serial.println("Gagal terus -- paksa WiFi reconnect...");
        WiFi.disconnect();
        delay(200);
        connectWiFi(); // blocking sampai WiFi beneran nyambung lagi (atau portal setup dibuka)
        failCount = 0;
      }
    }
  }
}

// Kirim heartbeat ke backend Vercel -- kasih tahu "saya masih hidup", SEKALIAN
// kirim data suhu (dummy/simulasi, karena ESP-01 sudah tidak punya pin sensor lagi).
// Field relay (status..status4) TIDAK ikut dikirim -- backend otomatis mempertahankan
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

  // Sensor PIR (gerak) & pintu/jendela -- DUMMY ACAK dulu (belum pasang sensor fisik).
  // Kalau nanti sudah pasang sensor asli, ganti dua baris ini dengan pembacaan pin GPIO
  // (mis. digitalRead(PIR_PIN) == HIGH -> "ON"), tidak perlu ubah apa pun di backend/app.
  const char* pirDummy    = random(0, 10) < 2 ? "ON" : "OFF"; // ~20% peluang "ada gerak"
  const char* doorWinDummy = random(0, 10) < 3 ? "ON" : "OFF"; // ~30% peluang "terbuka"

  StaticJsonDocument<320> doc; // dinaikkan sedikit karena field bertambah (status_pir, status_dor_win)
  doc["device"] = deviceName;
  doc["suhu"] = suhuText1;
  doc["suhu2"] = suhuText2;
  doc["suhu3"] = suhuText3;
  doc["cuaca"] = cuaca;
  doc["status_pir"] = pirDummy;
  doc["status_dor_win"] = doorWinDummy;

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
  for (int i = 0; i < NUM_AUTO; i++) {
    digitalWrite(RELAY_PINS[i], HIGH); // HIGH dulu sebelum jadi OUTPUT (default OFF, aman saat boot)
    pinMode(RELAY_PINS[i], OUTPUT);
  }

  if (STATUS_LED >= 0) {
    digitalWrite(STATUS_LED, HIGH);
    pinMode(STATUS_LED, OUTPUT);
  }

  Serial.begin(115200);
  randomSeed(micros()); // biar nilai suhu dummy tidak sama persis tiap boot

  // Nama device & nama WiFi setup, unik per alat (dari Chip ID ESP)
  snprintf(deviceName, sizeof(deviceName), "ESP-%06lX", (unsigned long)ESP.getChipId());
  snprintf(apName, sizeof(apName), "SmartHome-%06lX", (unsigned long)ESP.getChipId());
  snprintf(mqttTopic, sizeof(mqttTopic), "smarthome/%s/status", deviceName);
  Serial.println();
  Serial.print("Device: ");
  Serial.println(deviceName);

  if (RESET_WIFI_ON_BOOT) {
    WiFi.persistent(true);
    WiFi.disconnect(true); // hapus WiFi tersimpan
    delay(1000);
  }

  connectWiFi();

  // Jam internet (NTP), zona WIB = UTC+7. Dipakai untuk jadwal auto.
  configTime(7 * 3600, 0, "pool.ntp.org", "time.google.com");

  secureMqttClient.setInsecure();
  secureMqttClient.setBufferSizes(512, 512); // hemat memori -- penting karena koneksi ini nyala terus
  mqttClient.setServer(MQTT_HOST, MQTT_PORT);
  mqttClient.setCallback(onMqttMessage);
  mqttClient.setBufferSize(768); // payload MQTT: status relay + jadwal auto

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

  if (millis() - lastAutoCheck >= 1000) {
    lastAutoCheck = millis();
    checkAutoSchedules();
  }
  reportPendingRelays();

  if (millis() - lastHeartbeat >= HEARTBEAT_INTERVAL_MS) {
    lastHeartbeat = millis();
    sendHeartbeat();
    mqttClient.loop(); // langsung proses MQTT lagi setelah heartbeat selesai
  }
}
