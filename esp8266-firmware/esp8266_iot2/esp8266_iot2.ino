/*
  ESP-01 (ESP8266) -- semua lewat MQTT (EMQX Cloud). TIDAK ADA lagi heartbeat HTTP ke Vercel.

  1. STATUS ONLINE/OFFLINE = MQTT Last Will and Testament (LWT) + pesan retained
     Topic: smarthome/<device>/online
     - Saat konek, ESP mendaftarkan "wasiat" ke broker: kalau ESP putus/mati tanpa pamit,
       BROKER sendiri yang mempublish "offline" (retained).
     - Begitu berhasil konek, ESP mempublish "online" (retained).
     - Deteksi putus kira-kira 1,5 x keepalive (keepalive 30 dtk -> sekitar 45 dtk).
     Tidak ada request periodik ke Vercel sama sekali.

  2. KONTROL RELAY -- subscribe smarthome/<device>/status (retained), sama seperti sebelumnya.

  3. TELEMETRY (suhu, cuaca, PIR, pintu) -- dipublish lewat MQTT ke smarthome/<device>/telemetry
     (retained, JSON) tiap TELEMETRY_INTERVAL_MS (5 menit) dan sekali tiap berhasil konek.
     Suhu di sini DUMMY (angka acak), karena ESP-01 tidak punya pin sensor tersisa.

  4. AUTO (jadwal lampu) -- jadwal diterima lewat MQTT (retained). Jamnya dicek di ESP sendiri
     pakai jam NTP (WIB), jadi tetap jalan walau internet/server putus. Saat relay berganti
     karena jadwal, ESP melapor ke backend lewat HTTPS (POST RELAY_REPORT_URL) -- jarang terjadi
     (hanya saat jadwal berganti), dan perlu supaya notifikasi push/Telegram + snapshot retained
     di broker ikut diperbarui.

  5. SETUP WiFi -- SSID/password tidak ditanam. Pertama kali menyala (atau WiFi lama tidak ketemu
     > 2 menit), ESP membuat WiFi "SmartHome-<ChipID>" dengan halaman setup otomatis.
     Nama device otomatis dari Chip ID ("ESP-<ChipID>").

  Library: WiFiManager (tzapu), PubSubClient (Nick O'Leary), ArduinoJson (Benoit Blanchon),
  ESP8266WiFi, WiFiClientSecure, ESP8266HTTPClient (bawaan board package ESP8266)
*/

#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
#include <WiFiManager.h>   // by tzapu -- portal setup WiFi
#include <time.h>
#define MQTT_KEEPALIVE 30 // detik; menentukan kecepatan broker menandai device OFFLINE (~1,5x nilai ini)
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

// Backend Vercel -- HANYA untuk lapor relay yang berubah karena jadwal auto (jarang terjadi)
const char* RELAY_REPORT_URL = "https://backend-esp-8266.vercel.app/api/heartbeat";

// Broker EMQL Cloud (dipakai buat kontrol relay real-time)
const char* MQTT_HOST = "zd23e698.ala.eu-central-1.emqxsl.com";
const int   MQTT_PORT = 8883;
const char* MQTT_USER = "SmartHomeIoT";
const char* MQTT_PASS = "rayyanazka";

// Interval publish telemetry ke MQTT (ms). Tidak berpengaruh ke status online/offline (itu urusan LWT).
const unsigned long TELEMETRY_INTERVAL_MS = 300000UL; // 5 menit
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

char mqttTopic[64];       // subscribe: kontrol relay + jadwal
char onlineTopic[64];     // LWT: "online" / "offline" (retained)
char telemetryTopic[64];  // publish: suhu, cuaca, PIR, pintu (retained)
unsigned long lastTelemetry = 0;
bool telemetryDue = false; // true = kirim telemetry secepatnya (mis. baru konek)

// ---------------------------------------------------------------- Diagnostik, self-heal, simpan relay
// Log tampil di Serial Monitor (115200). "blok terbesar" = potongan memori kosong terbesar yang
// tersambung; ini angka yang menentukan apakah koneksi TLS baru bisa dibuat (bukan total heap bebas).
const unsigned long DIAG_INTERVAL_MS = 30000UL;
// Laporan relay lewat HTTPS dilewati kalau memori terlalu terpecah untuk koneksi TLS baru
// (nanti dicoba lagi). Sesuaikan nilainya setelah membaca log [DIAG].
const uint32_t MIN_FREE_BLOCK = 5000;  // byte

unsigned long lastDiagLog = 0;
unsigned long wifiDisconnectCount = 0;
int  lastDisconnectReason = 0;
unsigned long lastRelaySwitchMs = 0;
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
  Serial.printf_P(PSTR("[DIAG] uptime %lus | WiFi %s RSSI %d dBm | MQTT %s | heap %u | blok terbesar %u | WiFi putus %lu kali (alasan terakhir %d)\n"),
                  millis() / 1000,
                  WiFi.status() == WL_CONNECTED ? "OK" : "PUTUS", WiFi.RSSI(),
                  mqttClient.connected() ? "OK" : "PUTUS",
                  (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxFreeBlockSize(),
                  wifiDisconnectCount, lastDisconnectReason);
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

// Lapor status relay ke backend (POST RELAY_REPORT_URL, field status/status2). Return true kalau sukses.
bool postRelayState(int idx) {
  if (WiFi.status() != WL_CONNECTED) return false;
  if (ESP.getMaxFreeBlockSize() < MIN_FREE_BLOCK) return false; // memori terpecah, coba lagi nanti

  WiFiClientSecure client;
  client.setInsecure();
  client.setBufferSizes(512, 512);

  HTTPClient http;
  http.begin(client, RELAY_REPORT_URL); // laporan relay (tanpa kode aktivasi)
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

// Coba konek MQTT secara NON-BLOCKING.
// Kalau broker sedang bermasalah, loop() tetap berjalan sehingga:
// - AUTO tetap dicek
// - relay tetap bisa diproses
// - ESP tidak "hang" menunggu MQTT.
unsigned long lastMqttAttempt = 0;
const unsigned long MQTT_RETRY_INTERVAL_MS = 5000UL;

void tryConnectMqtt() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (mqttClient.connected()) return;

  // Jangan mencoba terlalu sering.
  if (millis() - lastMqttAttempt < MQTT_RETRY_INTERVAL_MS) return;
  lastMqttAttempt = millis();

  Serial.print(F("Menghubungkan ke EMQX Cloud..."));

  char clientId[40];
  snprintf(clientId, sizeof(clientId), "esp8266-%s", deviceName);

  // LWT: kalau koneksi putus tanpa DISCONNECT, broker mempublish "offline" (retained) atas nama ESP.
  if (mqttClient.connect(clientId, MQTT_USER, MQTT_PASS, onlineTopic, 1, true, "offline")) {
    Serial.println(F("Terhubung"));
    mqttClient.publish(onlineTopic, "online", true); // retained -> status terbaru untuk subscriber mana pun
    telemetryDue = true;                             // kirim telemetry segera setelah konek

    if (mqttClient.subscribe(mqttTopic)) {
      Serial.print(F("Subscribe ke: "));
      Serial.println(mqttTopic);
    } else {
      Serial.println(F("Subscribe gagal."));
    }

    statusLed(false);
  } else {
    Serial.printf_P(
      PSTR("Gagal, rc=%d | heap %u | blok terbesar %u -> coba lagi 5 detik lagi\n"),
      mqttClient.state(),
      (unsigned)ESP.getFreeHeap(),
      (unsigned)ESP.getMaxFreeBlockSize()
    );

    // Tidak ada delay panjang dan TIDAK disconnect WiFi.
    statusLed(true);
  }
}

// Publish telemetry (suhu dummy, cuaca dummy, PIR, pintu) ke MQTT sebagai JSON retained.
// Field relay (status..status4) TIDAK ikut dikirim -- itu urusan topic kontrol relay.
void publishTelemetry() {
  if (!mqttClient.connected()) return; // dicoba lagi begitu MQTT tersambung (telemetryDue tetap true)

  // Suhu + kelembaban dummy, digabung jadi satu teks. Pakai buffer tetap, bukan String.
  char suhuText1[24], suhuText2[24], suhuText3[24];
  int t1 = random(250, 350), t2 = random(250, 350), t3 = random(250, 350); // per 0,1 derajat
  snprintf(suhuText1, sizeof(suhuText1), "%d.%dC, %d%%RH", t1 / 10, t1 % 10, (int)random(40, 90));
  snprintf(suhuText2, sizeof(suhuText2), "%d.%dC, %d%%RH", t2 / 10, t2 % 10, (int)random(40, 90));
  snprintf(suhuText3, sizeof(suhuText3), "%d.%dC, %d%%RH", t3 / 10, t3 % 10, (int)random(40, 90));

  // Status cuaca dummy, acak salah satu
  const char* cuacaOptions[] = { "Cerah", "Hujan", "Mendung" };
  const char* cuaca = cuacaOptions[random(0, 3)];

  // Sensor PIR & pintu/jendela -- DUMMY ACAK dulu (belum ada sensor fisik).
  // Nanti ganti dengan digitalRead(pin) tanpa perlu ubah topic/format.
  const char* pirDummy     = random(0, 10) < 2 ? "ON" : "OFF";
  const char* doorWinDummy = random(0, 10) < 3 ? "ON" : "OFF";

  StaticJsonDocument<320> doc;
  doc["device"] = deviceName;
  doc["suhu"] = suhuText1;
  doc["suhu2"] = suhuText2;
  doc["suhu3"] = suhuText3;
  doc["cuaca"] = cuaca;
  doc["status_pir"] = pirDummy;
  doc["status_dor_win"] = doorWinDummy;

  char payload[300];
  size_t payloadLen = serializeJson(doc, payload, sizeof(payload));

  if (mqttClient.publish(telemetryTopic, (const uint8_t*)payload, payloadLen, true)) {
    telemetryDue = false;
    Serial.print(F("Telemetry terkirim: "));
    Serial.println(payload);
  } else {
    Serial.println(F("Telemetry gagal dipublish, coba lagi nanti."));
  }
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
  snprintf(onlineTopic, sizeof(onlineTopic), "smarthome/%s/online", deviceName);
  snprintf(telemetryTopic, sizeof(telemetryTopic), "smarthome/%s/telemetry", deviceName);
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
  mqttClient.setKeepAlive(MQTT_KEEPALIVE);

  tryConnectMqtt();
  lastTelemetry = millis();
}

void loop() {
  diagLoop();
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  }

  if (!mqttClient.connected()) {
    tryConnectMqtt();
  }
  if (mqttClient.connected()) mqttClient.loop();

  if (millis() - lastAutoCheck >= 1000) {
    lastAutoCheck = millis();
    checkAutoSchedules();
  }
  reportPendingRelays();

  if (millis() - lastTelemetry >= TELEMETRY_INTERVAL_MS) {
    lastTelemetry = millis();
    telemetryDue = true;
  }
  if (telemetryDue) publishTelemetry();
}
