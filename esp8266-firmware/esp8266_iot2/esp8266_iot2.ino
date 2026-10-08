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

// ---------------------------------------------------------------- Diagnostik, self-heal, simpan relay
// Log tampil di Serial Monitor (115200). "blok terbesar" = potongan memori kosong terbesar yang
// tersambung; ini angka yang menentukan apakah koneksi TLS baru bisa dibuat (bukan total heap bebas).
const unsigned long DIAG_INTERVAL_MS = 30000UL;
// Restart otomatis kalau heartbeat gagal terus (WiFi tersambung tapi HTTPS tidak bisa) atau memori
// terlalu terpecah. Nilai MIN_FREE_BLOCK sebaiknya disesuaikan setelah membaca log [HB]/[DIAG].
const int      MAX_HEARTBEAT_FAILS = 5;     // berturut-turut (sekitar 75 detik) -> restart
const uint32_t MIN_FREE_BLOCK      = 5000;  // byte; di bawah ini heartbeat HTTPS dilewati & dihitung gagal

unsigned long lastDiagLog = 0;
unsigned long wifiDisconnectCount = 0;
int  lastDisconnectReason = 0;
unsigned long lastRelaySwitchMs = 0;
int  heartbeatFails = 0;
bool firstEvalSilent = false;               // true = habis restart lunak, evaluasi jadwal pertama tidak mengubah relay
WiFiEventHandler onDiscHandler, onGotIpHandler;

// Relay disimpan di RTC memory supaya lampu tidak padam saat alat restart sendiri (restart lunak/watchdog/crash).
// RTC memory TIDAK bertahan saat listrik mati -- di kasus itu alat mulai dari relay OFF seperti biasa.
const uint32_t RTC_OFFSET = 96;            // blok 4-byte; jauh dari area yang dipakai bootloader
const uint32_t RTC_MAGIC  = 0x52454C59UL;  // "RELY"

void saveRelaysToRtc() {
  uint32_t d[3];
  d[0] = RTC_MAGIC;
  d[1] = 0;
  for (int i = 0; i < NUM_AUTO; i++) if (digitalRead(RELAY_PINS[i]) == LOW) d[1] |= (1UL << i);
  d[2] = d[0] ^ d[1] ^ 0xA5A5A5A5UL;
  ESP.rtcUserMemoryWrite(RTC_OFFSET, d, sizeof(d));
}

// Return true kalau ada data relay yang valid (hanya dipercaya setelah restart lunak/crash/watchdog).
bool readRelaysFromRtc(uint32_t& mask) {
  rst_info* ri = ESP.getResetInfoPtr();
  if (ri->reason == REASON_DEFAULT_RST || ri->reason == REASON_EXT_SYS_RST || ri->reason == REASON_DEEP_SLEEP_AWAKE) return false;
  uint32_t d[3];
  if (!ESP.rtcUserMemoryRead(RTC_OFFSET, d, sizeof(d))) return false;
  if (d[0] != RTC_MAGIC || d[2] != (d[0] ^ d[1] ^ 0xA5A5A5A5UL)) return false;
  mask = d[1];
  return true;
}

void restartWithLog(const char* why) {
  Serial.printf_P(PSTR("[RESTART] %s | heap %u | blok terbesar %u | uptime %lus\n"),
                  why, (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxFreeBlockSize(), millis() / 1000);
  delay(200);
  ESP.restart();
}

void diagSetup() {
  Serial.printf_P(PSTR("\n[BOOT] penyebab restart: %s\n"), ESP.getResetReason().c_str());
  onDiscHandler = WiFi.onStationModeDisconnected([](const WiFiEventStationModeDisconnected& e) {
    wifiDisconnectCount++;
    lastDisconnectReason = (int)e.reason;
    long sinceRelay = lastRelaySwitchMs ? (long)((millis() - lastRelaySwitchMs) / 1000) : -1;
    Serial.printf_P(PSTR("[WIFI] PUTUS #%lu, alasan %d | uptime %lus | relay terakhir berubah %ld dtk lalu (-1 = belum)\n"),
                    wifiDisconnectCount, lastDisconnectReason, millis() / 1000, sinceRelay);
  });
  onGotIpHandler = WiFi.onStationModeGotIP([](const WiFiEventStationModeGotIP& e) {
    Serial.printf_P(PSTR("[WIFI] dapat IP %s, RSSI %d dBm\n"), e.ip.toString().c_str(), WiFi.RSSI());
  });
}

void diagLoop() {
  if (millis() - lastDiagLog < DIAG_INTERVAL_MS) return;
  lastDiagLog = millis();
  Serial.printf_P(PSTR("[DIAG] uptime %lus | WiFi %s RSSI %d dBm | MQTT %s | heap %u | blok terbesar %u | WiFi putus %lu kali (alasan terakhir %d) | HB gagal berturut %d\n"),
                  millis() / 1000,
                  WiFi.status() == WL_CONNECTED ? "OK" : "PUTUS", WiFi.RSSI(),
                  mqttClient.connected() ? "OK" : "PUTUS",
                  (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxFreeBlockSize(),
                  wifiDisconnectCount, lastDisconnectReason, heartbeatFails);
}

void statusLed(bool on) {
  if (STATUS_LED >= 0) digitalWrite(STATUS_LED, on ? LOW : HIGH);
}

// Buka portal setup WiFi (WiFi milik alat) sampai pelanggan mengisi WiFi rumahnya.
// Kalau belum pernah ada WiFi tersimpan, portal terbuka TANPA batas waktu.
void openConfigPortal() {
  bool hasSaved = WiFi.SSID().length() > 0;

  Serial.println();
  Serial.print(F("Portal setup WiFi dibuka. Sambungkan HP ke WiFi: "));
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
      Serial.print(F("Menghubungkan ke WiFi tersimpan: "));
      Serial.println(WiFi.SSID());
      WiFi.begin(); // pakai kredensial yang tersimpan di flash

      unsigned long t0 = millis();
      while (WiFi.status() != WL_CONNECTED && millis() - t0 < WIFI_RETRY_BEFORE_PORTAL_MS) {
        Serial.print(F("."));
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

  WiFi.setSleepMode(WIFI_NONE_SLEEP); // matikan power-save WiFi: koneksi lebih stabil
  Serial.print(F("WiFi terhubung. IP: "));
  Serial.println(WiFi.localIP());
}

// Pasang status relay ke pin (aktif LOW)
void setRelay(int idx, bool on) {
  digitalWrite(RELAY_PINS[idx], on ? LOW : HIGH);
  lastRelaySwitchMs = millis();
  saveRelaysToRtc();
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
      // Habis restart lunak: relay sudah dipulihkan ke kondisi terakhir, jadi jangan ditimpa
      // (mis. relay yang dinyalakan manual di luar jam jadwal). Jadwal berlaku lagi di pergantian berikutnya.
      bool silent = (a.lastDesired == -1) && firstEvalSilent;
      a.lastDesired = desired;
      if (!silent) {
        setRelay(i, desired == 1);
        a.pendingReport = true;
        Serial.print(F("AUTO "));
        Serial.print(i + 1);
        Serial.print(F(" -> "));
        Serial.println(desired ? "ON" : "OFF");
      }
    }
  }
  firstEvalSilent = false;
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
  char payload[128];
  size_t payloadLen = serializeJson(doc, payload, sizeof(payload));

  int code = http.POST((uint8_t*)payload, payloadLen);
  Serial.print(F("Lapor relay "));
  Serial.print(idx + 1);
  Serial.print(F(" -> kode "));
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
  Serial.print(F("Pesan masuk ["));
  Serial.print(topic);
  Serial.print(F("]: "));
  Serial.write(payload, length);
  Serial.println();

  StaticJsonDocument<512> doc;
  DeserializationError err = deserializeJson(doc, (const char*)payload, length);
  if (err) {
    Serial.println(F("Gagal parse JSON."));
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
        Serial.print(F("Jadwal "));
        Serial.print(AUTO_FIELDS[i]);
        Serial.print(F(": "));
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
    Serial.print(F("Relay "));
    Serial.print(i + 1);
    Serial.print(F(" -> "));
    Serial.println(shouldBeOn ? "ON" : "OFF");
  }
}

void connectMqtt() {
  while (!mqttClient.connected()) {
    Serial.print(F("Menghubungkan ke HiveMQ..."));

    char clientId[40];
    snprintf(clientId, sizeof(clientId), "esp8266-%s", deviceName);

    if (mqttClient.connect(clientId, MQTT_USER, MQTT_PASS)) {

      Serial.println(F("Terhubung"));

      mqttClient.subscribe(mqttTopic);

      Serial.print(F("Subscribe ke: "));
      Serial.println(mqttTopic);

      statusLed(false);

    } else {

      Serial.printf_P(
        PSTR("Gagal, rc=%d | heap %u | blok terbesar %u"),
        mqttClient.state(),
        (unsigned)ESP.getFreeHeap(),
        (unsigned)ESP.getMaxFreeBlockSize()
      );

      Serial.println(F(" -> coba lagi 2 detik lagi"));

      statusLed(true);
      delay(100);
      statusLed(false);

      delay(1900);

      // JANGAN disconnect WiFi di sini.
      // Biarkan MQTT mencoba reconnect sendiri.
    }
  }
}

// Kirim heartbeat ke backend Vercel -- kasih tahu "saya masih hidup", SEKALIAN
// kirim data suhu (dummy/simulasi, karena ESP-01 sudah tidak punya pin sensor lagi).
// Field relay (status..status4) TIDAK ikut dikirim -- backend otomatis mempertahankan
// nilai lama untuk itu, cuma suhu & last_heartbeat yang ter-refresh.
void sendHeartbeat() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("WiFi tidak terhubung, skip heartbeat."));
    return;
  }

  uint32_t heapBefore = ESP.getFreeHeap();
  uint32_t blockBefore = ESP.getMaxFreeBlockSize();

  // Memori terlalu terpecah untuk membuka koneksi TLS baru -> lewati, hitung gagal.
  if (blockBefore < MIN_FREE_BLOCK) {
    heartbeatFails++;
    Serial.printf_P(PSTR("[HB] DILEWATI: blok terbesar %u < %u (heap %u) | gagal berturut %d\n"),
                    (unsigned)blockBefore, (unsigned)MIN_FREE_BLOCK, (unsigned)heapBefore, heartbeatFails);
    if (heartbeatFails >= MAX_HEARTBEAT_FAILS) restartWithLog("memori terpecah, heartbeat gagal terus");
    return;
  }

  WiFiClientSecure client;
  client.setInsecure();
  client.setBufferSizes(512, 512); // hemat memori, penting untuk ESP8266

  HTTPClient http;
  http.begin(client, HEARTBEAT_URL);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(10000); // dilonggarkan -- MQTT sekarang toleran (keepalive 60s), jadi aman dikasih waktu lebih

  // Suhu + kelembaban dummy, digabung jadi satu teks (bukan kolom terpisah). Pakai buffer tetap, bukan String.
  char suhuText1[24], suhuText2[24], suhuText3[24];
  int t1 = random(250, 350), t2 = random(250, 350), t3 = random(250, 350); // per 0,1 derajat
  snprintf(suhuText1, sizeof(suhuText1), "%d.%dC, %d%%RH", t1 / 10, t1 % 10, (int)random(40, 90));
  snprintf(suhuText2, sizeof(suhuText2), "%d.%dC, %d%%RH", t2 / 10, t2 % 10, (int)random(40, 90));
  snprintf(suhuText3, sizeof(suhuText3), "%d.%dC, %d%%RH", t3 / 10, t3 % 10, (int)random(40, 90));

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

  char payload[300];
  size_t payloadLen = serializeJson(doc, payload, sizeof(payload));

  int httpCode = http.POST((uint8_t*)payload, payloadLen);
  if (httpCode > 0) {
    heartbeatFails = 0;
    Serial.print(F("Heartbeat terkirim, kode: "));
    Serial.print(httpCode);
    Serial.print(F(" | "));
    Serial.println(payload);
  } else {
    heartbeatFails++;
    Serial.printf_P(PSTR("Heartbeat gagal: %s | gagal berturut %d\n"), http.errorToString(httpCode).c_str(), heartbeatFails);
  }

  http.end();
  client.stop();
  delay(200); // kasih waktu stack WiFi/TLS "napas" sebelum lanjut proses MQTT

  // Sebelum vs sesudah: kalau angka "sesudah" terus lebih kecil dari "sebelum" dari siklus ke siklus, ada kebocoran.
  Serial.printf_P(PSTR("[HB] heap %u -> %u | blok terbesar %u -> %u\n"),
                  (unsigned)heapBefore, (unsigned)ESP.getFreeHeap(), (unsigned)blockBefore, (unsigned)ESP.getMaxFreeBlockSize());

  if (heartbeatFails >= MAX_HEARTBEAT_FAILS) restartWithLog("heartbeat gagal berturut-turut");
}

void setup() {
  // Habis restart lunak/crash/watchdog: pulihkan relay ke kondisi terakhir (lampu tidak padam).
  // Habis listrik mati/dicolok: semua relay OFF seperti biasa.
  uint32_t restoredMask = 0;
  bool restored = readRelaysFromRtc(restoredMask);
  firstEvalSilent = restored;
  for (int i = 0; i < NUM_AUTO; i++) {
    bool on = restored && ((restoredMask >> i) & 1UL);
    digitalWrite(RELAY_PINS[i], on ? LOW : HIGH); // atur level dulu sebelum jadi OUTPUT (default OFF, aman saat boot)
    pinMode(RELAY_PINS[i], OUTPUT);
  }

  if (STATUS_LED >= 0) {
    digitalWrite(STATUS_LED, HIGH);
    pinMode(STATUS_LED, OUTPUT);
  }

  Serial.begin(115200);
  diagSetup();
  if (restored) Serial.printf_P(PSTR("[BOOT] relay dipulihkan dari RTC (mask %u)\n"), (unsigned)restoredMask);
  randomSeed(micros()); // biar nilai suhu dummy tidak sama persis tiap boot

  // Nama device & nama WiFi setup, unik per alat (dari Chip ID ESP)
  snprintf(deviceName, sizeof(deviceName), "ESP-%06lX", (unsigned long)ESP.getChipId());
  snprintf(apName, sizeof(apName), "SmartHome-%06lX", (unsigned long)ESP.getChipId());
  snprintf(mqttTopic, sizeof(mqttTopic), "smarthome/%s/status", deviceName);
  Serial.println();
  Serial.print(F("Device: "));
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
  diagLoop();
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
