# ESP8266 → HTTPS → Vercel API → Neon PostgreSQL

Project terpisah dari "IoT Industrial System". Alurnya: **push berbasis MQTT** untuk kontrol
relay, dan **heartbeat REST API** untuk status online/offline — semuanya diakses app Android
lewat REST API biasa (sama seperti suhu & relay), tidak perlu connect langsung ke broker.

```
[App Android] --POST (kontrol)--> [Backend Vercel] --UPDATE--> [Neon: iot2]
      ^                                  |
      |                            PUBLISH (MQTT)
      | GET (status_device,                |
      |  dihitung dari waktu       [HiveMQ Cloud broker]
      |  heartbeat terakhir)              |
      |                            SUBSCRIBE (push instan)
      |                                   |
      +------ GET /api/status ---- [ESP8266 -> LED]
                                          |
                                   POST heartbeat tiap 15s
                                          |
                                   [Backend Vercel] --UPDATE time--> [Neon: iot2]
```

Tabel `iot2` — **1 baris per device** (di-update terus, bukan nambah baris baru tiap kejadian):

| kolom                          | keterangan                                  |
|---------------------------------|----------------------------------------------|
| `id`                            | auto increment                                |
| `device`                        | nama/ID alat ESP8266, otomatis dari Chip ID mis. `ESP-A1B2C3` (UNIK — 1 baris per device) |
| `status`..`status4`             | status 4 relay/lampu berbeda: "ON" / "OFF"    |
| `status_pir`                     | sensor gerak (PIR): "ON" = ada gerak, "OFF" = aman -- BUKAN relay, read-only, diisi ESP |
| `status_dor_win`                 | sensor pintu/jendela: "ON" = buka, "OFF" = tutup -- BUKAN relay, read-only, diisi ESP |
| `suhu`, `suhu2`, `suhu3`        | 3 sensor suhu+kelembaban (teks gabungan, mis. "28.5C, 65%RH") |
| `auto1`, `auto2`                | jadwal lampu otomatis, format `"ON,18:00,06:00"` (aktif/nonaktif, jam nyala, jam mati — WIB) |
| `cuaca`                         | status cuaca: "Cerah" / "Hujan" / "Mendung"   |
| `last_heartbeat`                | waktu heartbeat TERAKHIR dari ESP (dipakai hitung online/offline) |
| `time`                          | otomatis diisi server tiap kali baris ini di-update |

## Fitur SENSOR (PIR & Pintu/Jendela) -- read-only

Dua kartu baru di app, di BAWAH Kontrol Relay, tanpa tombol apa pun (cuma menampilkan status):
- **Sensor Gerak (PIR)** — tampil "Ada Gerak" (merah) / "Aman" (hijau)
- **Pintu / Jendela** — tampil "Buka" (merah) / "Tutup" (hijau)

Beda dari relay: ini **bukan sesuatu yang bisa dinyalakan dari app**. Nilainya cuma laporan dari
ESP lewat `POST /api/heartbeat` (field `status_pir` dan `status_dor_win`, isinya tetap teks
"ON"/"OFF" di database, app yang menerjemahkan jadi teks Indonesia + warna).

**Status sekarang: DUMMY.** Firmware belum tersambung ke sensor fisik apa pun — nilainya diacak
setiap heartbeat (~20% "ada gerak", ~30% "terbuka"), cuma untuk mengetes tampilannya dulu.

**Kalau nanti mau pasang sensor asli** (mis. HC-SR501 untuk PIR, magnetic reed switch untuk
pintu/jendela): di firmware, cari fungsi `sendHeartbeat()`, ganti baris `pirDummy` dan
`doorWinDummy` dengan pembacaan pin GPIO sungguhan, misalnya:
```cpp
const char* pirDummy = digitalRead(PIR_PIN) == HIGH ? "ON" : "OFF";
```
Tidak perlu ubah apa pun di backend atau app untuk itu.

**Catatan:** Relay 5 (`status5`) sudah tidak dipakai lagi di app maupun backend (diganti sensor di
atas). Kolom `status5` di database dibiarkan apa adanya, tidak mengganggu apa pun.

## Fitur AUTO (jadwal nyala/mati lampu)

Menu **AUTO** di app (di atas Kontrol Relay) mengatur jadwal otomatis:
- **Auto 1 → Relay 1** (mis. lampu teras), **Auto 2 → Relay 2** (mis. lampu taman). Judul kartu
  berformat "Auto 1 (nama relay)" — nama relay ikut berubah otomatis kalau relay-nya di-rename
  di menu Kontrol Relay. Ada juga indikator lampu: ● Menyala (hijau) / ● Mati (abu-abu), diambil
  dari status relay yang dikendalikan.
- Tiap kartu punya switch aktif/nonaktif + tombol **Nyala** dan **Mati** (pilih jam & menit).
  Jadwal boleh melewati tengah malam (mis. nyala 18:00, mati 06:00).
- Jadwal disimpan di kolom `auto1` / `auto2` (format `"ON,18:00,06:00"`), dikirim ke ESP lewat
  MQTT retained.
- **Yang menjalankan jadwal adalah ESP8266 sendiri** (jam dari NTP, zona WIB), bukan server —
  jadi tetap jalan walau internet/server putus. Saat jam nyala/mati tiba, ESP menyalakan/
  mematikan relay lalu melapor ke `POST /api/data` supaya status di app ikut berubah.
- Kontrol manual tetap bisa dipakai di antara jam nyala dan mati (jadwal hanya bertindak saat
  waktu nyala/mati tiba, atau saat jadwal baru disimpan).
- Relay 2 di firmware memakai **GPIO2** (aktif LOW). Ubah `RELAY_PINS` di firmware kalau
  wiring-mu beda.

**Migrasi:** jalankan blok `ALTER TABLE ... auto1, auto2` di bagian paling bawah
`sql/create_table.sql`, deploy ulang backend, upload ulang firmware, lalu build ulang APK.

## Struktur folder

```
esp8266-iot2/
├── sql/                 -> script bikin tabel iot2 di Neon
├── backend/             -> REST API (Node.js + Express), deploy ke Vercel
├── esp8266-firmware/    -> kode Arduino (.ino) untuk ESP8266
└── android/             -> app Android "Smart Home IoT" (kontrol ON/OFF)
```

---

## 1. Buat tabel di Neon

Buka Neon Console → SQL Editor → jalankan isi `sql/create_table.sql`.

## 2. Jalankan & deploy backend

Sama seperti project sebelumnya:

```bash
cd backend
npm install
cp .env.example .env
```

Isi `.env`:
- `DATABASE_URL` — connection string Neon
- `DEVICE_CODE_SECRET` — teks acak panjang (min 32 karakter) untuk membuat kode aktivasi device. **Jangan diganti setelah ada alat terjual.**
- `ADMIN_API_KEY` — kunci admin untuk melihat daftar semua device (opsional; kosong = ditutup)
- `MQTT_HOST`, `MQTT_PORT`, `MQTT_USERNAME`, `MQTT_PASSWORD` — dari HiveMQ Cloud
  (tab "Overview" untuk host/port, tab "Access Management" untuk username/password)

Tes lokal dulu:
```bash
npm start
```

Kalau sudah oke, deploy ke Vercel (Add New Project → pilih repo ini → Root Directory: `backend`
→ tambahkan **SEMUA** environment variable di atas, termasuk yang MQTT_*, jangan cuma
DATABASE_URL saja → Deploy). Setelah dapat URL-nya (mis. `https://esp8266-iot2-backend.vercel.app`),
itu dipakai di app Android (`RetrofitClient.kt`).

### Endpoint yang tersedia

Akses per device memakai **kode aktivasi** (header `X-Device-Code`). Lihat bagian "Multi pelanggan" di bawah.

- `POST /api/claim` — `{"device":"ESP-A1B2C3","code":"ABCDE-FGHJK"}`, dipakai app saat pelanggan
  menambahkan device. Kode salah → 403.
- `POST /api/data` 🔒 — kirim `{"device":"...", ...field yang mau diubah}` (relay `status`..`status4`,
  jadwal `auto1`/`auto2`). Bisa sebagian field saja. Setelah tersimpan, backend publish ke MQTT
  topic `smarthome/<device>/status`. Butuh header `X-Device-Code`.
- `GET /api/status/:device` 🔒 — baris terakhir satu device (semua kolom + `status_device`).
- `GET /api/data/:device?limit=50` 🔒 — data terbaru satu device.
- `POST /api/heartbeat` — KHUSUS dipanggil ESP8266 tiap 15 detik, body
  `{"device":"...", "suhu":"..", "suhu2":"..", "suhu3":"..", "cuaca":".."}`. Boleh juga membawa
  `status` / `status2` ("ON"/"OFF") untuk melaporkan kondisi relay setelah jadwal auto berjalan.
  Dipakai buat hitung `status_device` (online/offline). Tanpa kode aktivasi, dan tidak
  mengembalikan data device.
- `GET /api/status` dan `GET /api/data` 🔒 (admin) — daftar semua device, butuh header
  `X-Admin-Key: <ADMIN_API_KEY>`. App Android TIDAK memakai ini.

## 3. Broker MQTT (HiveMQ Cloud)

1. Daftar gratis di hivemq.com (plan **Serverless**, tanpa kartu kredit).
2. Buat broker Cloud baru, catat **Host** dan **Port (8883, TLS)** dari tab Overview.
3. Di tab **Access Management**, buat username + password dengan permission
   "Publish and Subscribe".
4. Masukkan semua itu ke `.env` backend (lokal) DAN ke Environment Variables di
   dashboard Vercel (production).

## 4. Upload firmware ke ESP8266

1. Buka `esp8266-firmware/esp8266_iot2.ino` di Arduino IDE.
2. Install board package **ESP8266** (kalau belum): File → Preferences → Additional Board
   Manager URLs → tambahkan `http://arduino.esp8266.com/stable/package_esp8266com_index.json`
   → Tools → Board → Boards Manager → cari "esp8266" → Install.
3. Install 3 library lewat Library Manager:
   - **WiFiManager** (by tzapu)
   - **PubSubClient** (by Nick O'Leary)
   - **ArduinoJson** (by Benoit Blanchon)
4. Edit di bagian atas file:
   - `MQTT_HOST`, `MQTT_PORT`, `MQTT_USER`, `MQTT_PASS` → sama seperti yang diisi di
     `.env` backend
   - `HEARTBEAT_URL`, `DATA_URL` → URL backend Vercel
   - **WiFi tidak diisi di firmware** dan **nama device juga tidak** — keduanya otomatis
     (lihat bagian "Setup WiFi & nama device" di bawah).
5. Pilih board yang sesuai (Tools → Board → mis. "Generic ESP8266 Module").
6. Klik Upload.
7. Buka Serial Monitor (baud rate 115200) — akan muncul `Device: ESP-XXXXXX`. Setelah WiFi
   diatur, muncul "WiFi terhubung" lalu "Terhubung" (ke HiveMQ) dan "Subscribe ke:
   smarthome/ESP-XXXXXX/status".

### Setup WiFi & nama device (untuk produk yang dijual)

- **Nama device** dibuat otomatis dari Chip ID ESP8266, contoh `ESP-A1B2C3` — unik untuk tiap alat,
  tidak perlu diedit per unit. Device baru otomatis muncul di app begitu heartbeat pertamanya masuk.
  Catat/cetak nama ini di stiker produk (sama dengan nama WiFi setup di bawah).
- **Setup WiFi pertama kali** (captive portal):
  1. Nyalakan alat. Karena belum ada WiFi tersimpan, alat membuat WiFi bernama
     `SmartHome-A1B2C3` (terbuka, tanpa password, kecuali `AP_PASSWORD` diisi di firmware).
  2. Sambungkan HP ke WiFi itu — halaman setup terbuka otomatis (kalau tidak, buka `192.168.4.1`).
  3. Pilih WiFi rumah, isi password, Save. Alat menyimpan WiFi di flash dan tersambung sendiri.
- **Ganti WiFi / router baru:** kalau WiFi tersimpan gagal tersambung lebih dari 2 menit, alat
  otomatis membuka portal `SmartHome-XXXXXX` lagi (tertutup sendiri setelah 3 menit, lalu
  mencoba lagi). Jadi cukup matikan WiFi lama, tunggu sebentar, lalu setup ulang.
- **Testing di alat sendiri:** WiFi lama masih tersimpan di flash, jadi portal tidak muncul. Set
  `RESET_WIFI_ON_BOOT = true`, upload, biarkan sekali boot, lalu kembalikan ke `false` dan upload lagi.
- Alat lama yang memakai nama `ESP01_01` akan muncul sebagai device terpisah di app (baris lamanya
  tetap ada di database, bisa dihapus lewat Neon).

## 5. Cek data masuk

Setelah alat menyala dan WiFi diatur, cek lewat admin (ganti URL & kunci):

```bash
curl -H "X-Admin-Key: <ADMIN_API_KEY>" https://<url-vercel-kamu>/api/status
```

Device `ESP-XXXXXX` harus muncul. Lalu di app Android: tombol **+** → isi ID & kode aktivasi
(dari stiker) → device muncul. Tekan ON/OFF, relay harus langsung berubah.

## Multi pelanggan (tiap orang hanya melihat device miliknya)

- Server **tidak pernah** memberi daftar semua device ke app. App hanya menampilkan device yang
  ditambahkan pelanggan sendiri lewat tombol **+** (ID device + kode aktivasi dari stiker).
  Daftar itu disimpan di HP; tombol tempat sampah menghapus device dari app (alatnya tetap jalan).
- **Kode aktivasi** dibuat dari nama device + `DEVICE_CODE_SECRET` (HMAC), tidak disimpan di
  database. Setiap alat punya kode berbeda, dan tanpa kode yang benar server menolak baca/kontrol (403).
- **Cara paling mudah:** buka `tools/generator-kode.html` di browser, tempel kunci rahasia dan ID alat,
  kodenya langsung muncul (bisa disalin atau dicetak). Semua dihitung di browser, tidak dikirim ke mana pun.
- **Alternatif lewat terminal** — saat flashing tiap alat, catat `Device: ESP-XXXXXX` dari Serial Monitor, lalu:

  ```bash
  cd backend
  npm install
  npm run code -- ESP-A1B2C3 ESP-D4E5F6
  # ID: ESP-A1B2C3    Kode: AL5UH-F89JE
  ```

  (butuh `backend/.env` berisi `DEVICE_CODE_SECRET` yang SAMA dengan di Vercel). Cetak ID + kode di
  stiker/kemasan. Beberapa HP boleh menambahkan device yang sama (mis. keluarga) selama punya kodenya.

## 5. App Android "Smart Home IoT"

App Android di folder `android/` punya:
- Header "MQTT - Neon PostgreSQL - Vercel - Android"
- Kartu **Status Database** (koneksi app ke backend Vercel/Neon)
- Kartu **Status Device** — dihitung backend dari heartbeat terakhir ESP8266 (field
  `status_device` di response API): kalau lebih dari 35 detik tanpa heartbeat, otomatis
  "Offline". Dibaca lewat REST API biasa, sama seperti suhu & relay -- app tidak perlu
  simpan kredensial broker MQTT sama sekali.
- Combo box pilih device (hanya device yang sudah ditambahkan lewat tombol **+**; tombol tempat sampah = hapus dari app)
- 3 kartu suhu (suhu, suhu2, suhu3) — judul bisa di-edit (ikon pensil)
- 5 baris relay dengan Switch ON/OFF — nama relay juga bisa di-edit
- Auto-refresh data (suhu, status relay, status device) tiap 5 detik lewat REST API

Sebelum build, buka `android/app/src/main/java/com/smarthome/iot/RetrofitClient.kt`,
pastikan `BASE_URL` sudah sesuai URL Vercel backend kamu.

Bisa dibuild via Android Studio (buka folder `android/`, klik Run), atau otomatis lewat
GitHub Actions (tab Actions → "Build APK" → Run workflow → download APK dari Artifacts).

---

## Catatan

- Endpoint ini **belum pakai autentikasi** (sesuai permintaan, biar simpel dulu). Siapa pun yang
  tahu URL-nya bisa kirim data. Kalau nanti mau ditambah keamanan, saya bisa bantu tambahkan
  API key sederhana.
- `client.setInsecure()` di kode ESP8266 melewati verifikasi sertifikat HTTPS — umum dipakai
  untuk project hobi karena ESP8266 berat kalau harus simpan root CA lengkap.
