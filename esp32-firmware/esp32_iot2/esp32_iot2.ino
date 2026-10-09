/*
  ESP32 Smart Home -- kontrol 5 relay + 3x DHT22 + PIR + limit switch pintu/jendela + sensor cuaca.

  Jalur komunikasi (sama seperti versi ESP8266):
  1. MQTT (HiveMQ Cloud): subscribe smarthome/<nama device>/status -> kontrol relay real-time dari app
     + jadwal AUTO (auto1 -> Relay 1, auto2 -> Relay 2, dicek di ESP pakai jam NTP WIB).
  2. HTTPS (Vercel): POST /api/heartbeat tiap 15 detik berisi suhu, cuaca, status PIR, status pintu.
     Perubahan relay oleh jadwal AUTO / PIR juga dilaporkan lewat sini supaya tampilan app sinkron.
  3. Setup WiFi lewat portal (WiFiManager): tidak ada SSID/password di firmware.
     Nama device otomatis dari MAC: "ESP-XXXXXX" (format sama dengan versi ESP8266, jadi generator
     kode aktivasi tidak berubah).

  ---------------- PIN (ubah di bagian "PIN" di bawah kalau wiring-mu beda) ----------------
  Relay 1..5      : GPIO 23, 22, 21, 19, 18   (Relay 5 = output PIR)
  DHT22 #1,#2,#3  : GPIO 4, 16, 17
  PIR (HC-SR501)  : GPIO 27
  Limit switch    : GPIO 14  (satu kaki ke GPIO14, satu kaki ke GND; pakai pull-up internal)
  Raindrop (AO)   : GPIO 34  (ADC1, input-only)
  LDR (divider)   : GPIO 35  (ADC1, input-only)
  Sengaja TIDAK memakai GPIO 0, 2, 5, 12, 15 (strapping/boot) dan GPIO 6-11 (flash).
  Kenapa sensor analog di GPIO34/35: ADC2 tidak bisa dipakai saat WiFi menyala, ADC1 aman.

  Library (Library Manager, Arduino IDE), board: "ESP32 Dev Module":
  - WiFiManager (by tzapu)
  - PubSubClient (by Nick O'Leary)
  - ArduinoJson (by Benoit Blanchon)
  - DHT sensor library (by Adafruit) + Adafruit Unified Sensor
  Kalau muncul "Sketch too big": Tools -> Partition Scheme -> "Huge APP".
*/

#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <WiFiManager.h>   // by tzapu -- portal setup WiFi
#include <time.h>
#include "esp_wifi.h"
#include <esp_system.h>   // esp_reset_reason() untuk diagnostik
#define MQTT_KEEPALIVE 60
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <DHT.h>

// ------------------- GANTI SESUAI KEBUTUHANMU -------------------
// Password WiFi setup milik alat (untuk terhubung ke portal). Kosong = terbuka. Kalau diisi, min 8 karakter.
const char* AP_PASSWORD = "";
const unsigned long WIFI_RETRY_BEFORE_PORTAL_MS = 120000UL; // WiFi tersimpan gagal > 2 menit -> buka portal
const unsigned int  PORTAL_TIMEOUT_S = 180;
// Set true SEKALI untuk menghapus WiFi tersimpan (memaksa portal muncul), lalu kembalikan ke false.
const bool RESET_WIFI_ON_BOOT = false;

const char* HEARTBEAT_URL = "https://backend-esp-8266.vercel.app/api/heartbeat";

// Broker EMQL Cloud (dipakai buat kontrol relay real-time)
const char* MQTT_HOST = "zd23e698.ala.eu-central-1.emqxsl.com";
const int   MQTT_PORT = 8883;
const char* MQTT_USER = "SmartHomeIoT";
const char* MQTT_PASS = "rayyanazka";

const unsigned long HEARTBEAT_INTERVAL_MS = 15000; // backend anggap OFFLINE kalau > 35 detik tanpa heartbeat

// ---------------- PIN ----------------
const int NUM_RELAY = 5;
const int RELAY_PINS[NUM_RELAY]      = { 23, 22, 21, 19, 18 };
const char* RELAY_FIELDS[NUM_RELAY]  = { "status", "status2", "status3", "status4", "status5" };
const bool RELAY_ACTIVE_LOW = true;  // modul relay umumnya aktif LOW (LOW = nyala). Ganti false kalau modulmu aktif HIGH.
const int PIR_RELAY_IDX = 4;         // Relay 5 dikendalikan sensor PIR

const int DHT_PINS[3] = { 4, 16, 17 };
const int PIR_PIN     = 27;
const int DOOR_PIN    = 14;
const int RAIN_PIN    = 34;
const int LDR_PIN     = 35;

// ---------------- PENGATURAN SENSOR ----------------
// PIR: relay 5 menyala saat ada gerak, mati setelah tidak ada gerak selama waktu ini.
const unsigned long PIR_HOLD_MS   = 10000UL;
// HC-SR501 butuh ~30-60 detik pemanasan setelah dinyalakan (banyak sinyal palsu). Diabaikan selama ini.
const unsigned long PIR_WARMUP_MS = 30000UL;

// Limit switch dengan INPUT_PULLUP: ditekan (pintu tertutup) = LOW, dilepas (pintu terbuka) = HIGH.
// Kalau pemasanganmu terbalik, ganti jadi LOW.
const int DOOR_OPEN_LEVEL = HIGH;

// Sensor hujan FC-37/YL-83 (AO): kering = nilai tinggi (~4095), basah = nilai turun.
// Nilai < ambang ini dianggap HUJAN. KALIBRASI lewat Serial Monitor (lihat log "Cuaca: rain=.. ldr=..").
const int RAIN_WET_THRESHOLD = 2500;
// LDR dengan rangkaian: 3.3V -- LDR -- GPIO35 -- resistor 10k -- GND (makin terang, nilai makin TINGGI).
// Nilai >= ambang ini dianggap CERAH, di bawahnya MENDUNG. Kalibrasi juga lewat Serial Monitor.
const int LIGHT_BRIGHT_THRESHOLD = 2000;
// ------------------------------------------------------------------

DHT dht[3] = { DHT(DHT_PINS[0], DHT22), DHT(DHT_PINS[1], DHT22), DHT(DHT_PINS[2], DHT22) };

char deviceName[24]; // "ESP-XXXXXX" dari MAC
char apName[32];     // "SmartHome-XXXXXX"
char mqttTopic[64];

// Jadwal auto: auto1 -> relay index 0, auto2 -> relay index 1
const int NUM_AUTO = 2;
const char* AUTO_FIELDS[NUM_AUTO] = { "auto1", "auto2" };

struct AutoSchedule {
  bool enabled = false;
  int  onMinute = 18 * 60;
  int  offMinute = 6 * 60;
  char raw[24] = "";
  int  lastDesired = -1;
};
AutoSchedule autoSched[NUM_AUTO];

bool relayState[NUM_RELAY];         // status relay sebenarnya (true = nyala)
bool pendingReport[NUM_RELAY];      // perlu dilaporkan ke backend
unsigned long lastReportTry = 0;
unsigned long lastAutoCheck = 0;
unsigned long lastHeartbeat = 0;
bool forceHeartbeat = false;

// PIR
bool pirActive = false;
unsigned long lastMotionMs = 0;
unsigned long bootMs = 0;

// Pintu/jendela (di-debounce 50 ms)
bool doorOpen = false;
bool doorRawLast = false;
unsigned long doorChangedAt = 0;

WiFiClientSecure secureMqttClient;
PubSubClient mqttClient(secureMqttClient);

// ---------------------------------------------------------------- Diagnostik (log Serial)
// Untuk mencari penyebab WiFi putus / alat restart sendiri. Buka Serial Monitor (115200 baud).
const unsigned long DIAG_INTERVAL_MS = 30000UL; // ringkasan kondisi tiap 30 detik
unsigned long lastDiagLog = 0;
unsigned long wifiDisconnectCount = 0;
int lastDisconnectReason = 0;
unsigned long lastRelaySwitchMs = 0;            // diisi di setRelay(), untuk melihat apakah putus terjadi setelah relay berpindah

const char* resetReasonText(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:   return "POWERON (dicolok/dinyalakan)";
    case ESP_RST_SW:        return "SW (restart lewat kode)";
    case ESP_RST_PANIC:     return "PANIC (program crash)";
    case ESP_RST_INT_WDT:   return "INT_WDT (watchdog)";
    case ESP_RST_TASK_WDT:  return "TASK_WDT (watchdog, program macet)";
    case ESP_RST_WDT:       return "WDT (watchdog)";
    case ESP_RST_BROWNOUT:  return "BROWNOUT (tegangan drop -> cek catu daya!)";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_EXT:       return "EXT (tombol reset)";
    default:                return "LAINNYA / tidak diketahui";
  }
}

const char* disconnectReasonText(int reason) {
  switch (reason) {
    case 2:   return "AUTH_EXPIRE";
    case 8:   return "ASSOC_LEAVE (diputus alat/router)";
    case 15:  return "4WAY_HANDSHAKE_TIMEOUT (cek password/sinyal)";
    case 200: return "BEACON_TIMEOUT (sinyal router hilang)";
    case 201: return "NO_AP_FOUND (router tidak terlihat)";
    case 202: return "AUTH_FAIL (password salah)";
    case 203: return "ASSOC_FAIL";
    case 204: return "HANDSHAKE_TIMEOUT";
    case 205: return "CONNECTION_FAIL";
    default:  return "kode lain (cari di dokumentasi ESP-IDF: wifi reason code)";
  }
}

void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    wifiDisconnectCount++;
    lastDisconnectReason = info.wifi_sta_disconnected.reason;
    long sinceRelay = lastRelaySwitchMs ? (long)((millis() - lastRelaySwitchMs) / 1000) : -1;
    Serial.printf("[WIFI] PUTUS #%lu, alasan %d = %s | uptime %lus | relay terakhir berubah %ld dtk lalu (-1 = belum pernah)\n",
                  wifiDisconnectCount, lastDisconnectReason, disconnectReasonText(lastDisconnectReason),
                  millis() / 1000, sinceRelay);
  } else if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
    Serial.printf("[WIFI] dapat IP %s, RSSI %d dBm\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
  }
}

void logDiagnostics() {
  if (millis() - lastDiagLog < DIAG_INTERVAL_MS) return;
  lastDiagLog = millis();
  Serial.printf("[DIAG] uptime %lus | WiFi %s RSSI %d dBm | MQTT %s | heap bebas %u (terendah %u) | WiFi putus %lu kali (terakhir alasan %d)\n",
                millis() / 1000,
                WiFi.status() == WL_CONNECTED ? "OK" : "PUTUS",
                WiFi.RSSI(),
                mqttClient.connected() ? "OK" : "PUTUS",
                (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(),
                wifiDisconnectCount, lastDisconnectReason);
}

// ---------------------------------------------------------------- WiFi
bool hasSavedWiFi() {
  WiFi.mode(WIFI_STA);
  wifi_config_t conf;
  if (esp_wifi_get_config(WIFI_IF_STA, &conf) != ESP_OK) return false;
  return conf.sta.ssid[0] != 0;
}

void openConfigPortal() {
  bool hasSaved = hasSavedWiFi();
  Serial.println();
  Serial.print("Portal setup WiFi dibuka. Sambungkan HP ke WiFi: ");
  Serial.println(apName);

  WiFiManager wm;
  wm.setConfigPortalTimeout(hasSaved ? PORTAL_TIMEOUT_S : 0); // belum ada WiFi tersimpan -> tanpa batas waktu
  wm.setConnectTimeout(20);
  wm.setBreakAfterConfig(true);
  wm.startConfigPortal(apName, strlen(AP_PASSWORD) >= 8 ? AP_PASSWORD : NULL);
}

void connectWiFi() {
  while (WiFi.status() != WL_CONNECTED) {
    if (hasSavedWiFi()) {
      Serial.println("Menghubungkan ke WiFi tersimpan...");
      WiFi.begin(); // kredensial tersimpan di flash
      unsigned long t0 = millis();
      while (WiFi.status() != WL_CONNECTED && millis() - t0 < WIFI_RETRY_BEFORE_PORTAL_MS) {
        Serial.print(".");
        delay(500);
      }
      Serial.println();
    }
    if (WiFi.status() != WL_CONNECTED) {
      openConfigPortal();
    }
  }
  WiFi.setSleep(false); // matikan power-save WiFi: koneksi lebih stabil (konsumsi daya sedikit naik)
  Serial.print("WiFi terhubung. IP: ");
  Serial.println(WiFi.localIP());
}

// ---------------------------------------------------------------- Relay
void setRelay(int idx, bool on) {
  relayState[idx] = on;
  lastRelaySwitchMs = millis();
  digitalWrite(RELAY_PINS[idx], (on == RELAY_ACTIVE_LOW) ? LOW : HIGH);
}

// ---------------------------------------------------------------- AUTO (jadwal)
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

bool inWindow(int nowMin, int onMin, int offMin) {
  if (onMin == offMin) return false;
  if (onMin < offMin) return nowMin >= onMin && nowMin < offMin;
  return nowMin >= onMin || nowMin < offMin; // lewat tengah malam
}

// Aksi HANYA saat status "seharusnya" berubah, jadi kontrol manual dari app tetap bisa dipakai di antara jam nyala/mati.
void checkAutoSchedules() {
  time_t now = time(nullptr);
  if (now < 1700000000) return; // NTP belum sinkron

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
      pendingReport[i] = true;
      Serial.printf("AUTO %d -> %s\n", i + 1, desired ? "ON" : "OFF");
    }
  }
}

// ---------------------------------------------------------------- Laporan relay ke backend
bool postRelayState(int idx) {
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.begin(client, HEARTBEAT_URL);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(10000);

  StaticJsonDocument<192> doc;
  doc["device"] = deviceName;
  doc[RELAY_FIELDS[idx]] = relayState[idx] ? "ON" : "OFF";
  if (idx == PIR_RELAY_IDX) doc["status_pir"] = pirActive ? "ON" : "OFF"; // ikut dilaporkan supaya app langsung sinkron
  String payload;
  serializeJson(doc, payload);

  int code = http.POST(payload);
  Serial.printf("Lapor relay %d -> kode %d\n", idx + 1, code);

  http.end();
  client.stop();
  delay(100);
  return code >= 200 && code < 300;
}

void reportPendingRelays() {
  if (millis() - lastReportTry < 10000) return; // jeda antar percobaan (kalau gagal, coba lagi 10 detik kemudian)
  for (int i = 0; i < NUM_RELAY; i++) {
    if (pendingReport[i]) {
      lastReportTry = millis();
      if (postRelayState(i)) {
        pendingReport[i] = false;
        lastReportTry = 0; // sukses -> laporan berikutnya boleh langsung
      }
      return; // satu laporan per putaran, biar tidak menahan MQTT terlalu lama
    }
  }
}

// ---------------------------------------------------------------- Sensor
// PIR: gerak -> Relay 5 nyala + status_pir ON; tidak ada gerak selama PIR_HOLD_MS -> keduanya mati.
void handlePir() {
  if (millis() - bootMs < PIR_WARMUP_MS) return;

  if (digitalRead(PIR_PIN) == HIGH) {
    lastMotionMs = millis();
    if (!pirActive) {
      pirActive = true;
      setRelay(PIR_RELAY_IDX, true);
      pendingReport[PIR_RELAY_IDX] = true;
      Serial.println("PIR: ada gerak -> Relay 5 ON");
    }
  } else if (pirActive && millis() - lastMotionMs > PIR_HOLD_MS) {
    pirActive = false;
    setRelay(PIR_RELAY_IDX, false);
    pendingReport[PIR_RELAY_IDX] = true;
    Serial.println("PIR: aman -> Relay 5 OFF");
  }
}

// Limit switch pintu/jendela. Kalau berubah, heartbeat langsung dikirim (tidak menunggu 15 detik).
void handleDoor() {
  bool raw = (digitalRead(DOOR_PIN) == DOOR_OPEN_LEVEL);
  if (raw != doorRawLast) {
    doorRawLast = raw;
    doorChangedAt = millis();
  }
  if (raw != doorOpen && millis() - doorChangedAt >= 50) {
    doorOpen = raw;
    forceHeartbeat = true;
    Serial.println(doorOpen ? "Pintu/jendela: BUKA" : "Pintu/jendela: TUTUP");
  }
}

int readAdcAvg(int pin) {
  long sum = 0;
  for (int i = 0; i < 16; i++) {
    sum += analogRead(pin);
    delay(2);
  }
  return (int)(sum / 16);
}

// Cuaca dari sensor hujan + LDR: hujan (basah) > cerah (terang) > mendung (gelap).
const char* readWeather(int& rainRaw, int& ldrRaw) {
  rainRaw = readAdcAvg(RAIN_PIN);
  ldrRaw  = readAdcAvg(LDR_PIN);
  if (rainRaw < RAIN_WET_THRESHOLD) return "Hujan";
  if (ldrRaw >= LIGHT_BRIGHT_THRESHOLD) return "Cerah";
  return "Mendung";
}

// "26.2C, 70%RH" (format yang dibaca app). Gagal baca -> "--, --".
String dhtText(int i) {
  float t = dht[i].readTemperature();
  float h = dht[i].readHumidity();
  if (isnan(t) || isnan(h)) return "--, --";
  return String(t, 1) + "C, " + String((int)(h + 0.5f)) + "%RH";
}

// ---------------------------------------------------------------- Heartbeat
void sendHeartbeat() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi tidak terhubung, skip heartbeat.");
    return;
  }

  // Baca sensor dulu, baru buka koneksi HTTPS (biar koneksi tidak menggantung selama pembacaan)
  String s1 = dhtText(0);
  String s2 = dhtText(1);
  String s3 = dhtText(2);
  int rainRaw, ldrRaw;
  const char* cuaca = readWeather(rainRaw, ldrRaw);
  Serial.printf("Cuaca: rain=%d ldr=%d -> %s\n", rainRaw, ldrRaw, cuaca); // pakai ini untuk kalibrasi ambang

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.begin(client, HEARTBEAT_URL);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(10000);

  StaticJsonDocument<384> doc;
  doc["device"] = deviceName;
  doc["suhu"] = s1;
  doc["suhu2"] = s2;
  doc["suhu3"] = s3;
  doc["cuaca"] = cuaca;
  doc["status_pir"] = pirActive ? "ON" : "OFF";
  doc["status_dor_win"] = doorOpen ? "ON" : "OFF";
  // Status relay TIDAK ikut dikirim di sini (biar tidak menimpa perintah app yang baru masuk);
  // relay dilaporkan terpisah saat jadwal AUTO / PIR mengubahnya (postRelayState).

  String payload;
  serializeJson(doc, payload);

  int httpCode = http.POST(payload);
  if (httpCode > 0) {
    Serial.printf("Heartbeat terkirim, kode: %d | %s\n", httpCode, payload.c_str());
  } else {
    Serial.printf("Heartbeat gagal: %s\n", http.errorToString(httpCode).c_str());
  }

  http.end();
  client.stop();
  delay(100);
}

// ---------------------------------------------------------------- MQTT
void onMqttMessage(char* topic, byte* payload, unsigned int length) {
  String message;
  for (unsigned int i = 0; i < length; i++) message += (char)payload[i];

  Serial.print("Pesan masuk [");
  Serial.print(topic);
  Serial.print("]: ");
  Serial.println(message);

  StaticJsonDocument<512> doc;
  if (deserializeJson(doc, message)) {
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
        Serial.printf("Jadwal %s: %s\n", AUTO_FIELDS[i], raw);
      }
    }
  }

  // 2) Status ke-5 relay dari app
  for (int i = 0; i < NUM_RELAY; i++) {
    const char* st = doc[RELAY_FIELDS[i]];
    if (!st) continue;
    bool shouldBeOn = strcmp(st, "ON") == 0;
    setRelay(i, shouldBeOn);
    Serial.printf("Relay %d -> %s\n", i + 1, shouldBeOn ? "ON" : "OFF");
  }
}

void connectMqtt() {
  int failCount = 0;
  while (!mqttClient.connected()) {
    Serial.print("Menghubungkan ke EMQX Cloud...");
    String clientId = String("esp32-") + deviceName;

    if (mqttClient.connect(clientId.c_str(), MQTT_USER, MQTT_PASS)) {
      Serial.println("Terhubung");
      mqttClient.subscribe(mqttTopic);
      Serial.print("Subscribe ke: ");
      Serial.println(mqttTopic);
      failCount = 0;
    } else {
      failCount++;
      Serial.printf("Gagal, rc=%d -> coba lagi 2 detik lagi\n", mqttClient.state());
      delay(2000);

      if (failCount >= 3) { // WiFi kadang salah lapor "masih connected" -> paksa reconnect
        Serial.println("Gagal terus -- paksa WiFi reconnect...");
        WiFi.disconnect();
        delay(200);
        connectWiFi();
        failCount = 0;
      }
    }
  }
}

// ---------------------------------------------------------------- setup / loop
void setup() {
  // Relay: set level MATI dulu sebelum jadi OUTPUT (supaya tidak "klik" saat boot)
  for (int i = 0; i < NUM_RELAY; i++) {
    digitalWrite(RELAY_PINS[i], RELAY_ACTIVE_LOW ? HIGH : LOW);
    pinMode(RELAY_PINS[i], OUTPUT);
    relayState[i] = false;
    pendingReport[i] = false;
  }

  pinMode(PIR_PIN, INPUT);
  pinMode(DOOR_PIN, INPUT_PULLUP);
  pinMode(RAIN_PIN, INPUT);
  pinMode(LDR_PIN, INPUT);
  analogReadResolution(12); // 0..4095
  for (int i = 0; i < 3; i++) dht[i].begin();

  Serial.begin(115200);
  bootMs = millis();
  Serial.printf("\n[BOOT] penyebab restart: %s\n", resetReasonText(esp_reset_reason()));
  WiFi.onEvent(onWiFiEvent);

  // Nama device & nama WiFi setup: unik per alat, dari 3 byte terakhir MAC
  uint64_t mac = ESP.getEfuseMac();
  uint8_t b3 = (uint8_t)(mac >> 24), b4 = (uint8_t)(mac >> 32), b5 = (uint8_t)(mac >> 40);
  snprintf(deviceName, sizeof(deviceName), "ESP-%02X%02X%02X", b3, b4, b5);
  snprintf(apName, sizeof(apName), "SmartHome-%02X%02X%02X", b3, b4, b5);
  snprintf(mqttTopic, sizeof(mqttTopic), "smarthome/%s/status", deviceName);
  Serial.println();
  Serial.print("Device: ");
  Serial.println(deviceName);

  // Status awal pintu (tanpa memicu heartbeat paksa)
  doorRawLast = doorOpen = (digitalRead(DOOR_PIN) == DOOR_OPEN_LEVEL);

  if (RESET_WIFI_ON_BOOT) {
    WiFi.mode(WIFI_STA);
    WiFi.disconnect(true, true); // hapus WiFi tersimpan
    delay(1000);
  }

  connectWiFi();

  configTime(7 * 3600, 0, "pool.ntp.org", "time.google.com"); // WIB, untuk jadwal auto

  secureMqttClient.setInsecure();
  mqttClient.setServer(MQTT_HOST, MQTT_PORT);
  mqttClient.setCallback(onMqttMessage);
  mqttClient.setBufferSize(768);

  connectMqtt();
  sendHeartbeat();
  lastHeartbeat = millis();
}

void loop() {
  logDiagnostics();
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  }
  if (!mqttClient.connected()) {
    connectMqtt();
  }
  mqttClient.loop();

  handlePir();
  handleDoor();

  if (millis() - lastAutoCheck >= 1000) {
    lastAutoCheck = millis();
    checkAutoSchedules();
  }
  reportPendingRelays();

  bool due = millis() - lastHeartbeat >= HEARTBEAT_INTERVAL_MS;
  bool forced = forceHeartbeat && millis() - lastHeartbeat >= 2000; // pintu berubah -> kirim segera (maks tiap 2 detik)
  if (due || forced) {
    forceHeartbeat = false;
    lastHeartbeat = millis();
    sendHeartbeat();
    mqttClient.loop();
  }
}
