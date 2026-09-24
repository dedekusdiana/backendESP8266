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
| `device`                        | nama/ID alat ESP8266 (UNIK — 1 baris per device) |
| `status`..`status5`             | status 5 relay/lampu berbeda: "ON" / "OFF"    |
| `suhu`, `suhu2`, `suhu3`        | 3 sensor suhu+kelembaban (teks gabungan, mis. "28.5C, 65%RH") |
| `auto1`, `auto2`                | jadwal lampu otomatis, format `"ON,18:00,06:00"` (aktif/nonaktif, jam nyala, jam mati — WIB) |
| `cuaca`                         | status cuaca: "Cerah" / "Hujan" / "Mendung"   |
| `last_heartbeat`                | waktu heartbeat TERAKHIR dari ESP (dipakai hitung online/offline) |
| `time`                          | otomatis diisi server tiap kali baris ini di-update |

## Fitur AUTO (jadwal nyala/mati lampu)

Menu **AUTO** di app (di atas Kontrol Relay) mengatur jadwal otomatis:
- **Auto 1 → Relay 1** (mis. lampu teras), **Auto 2 → Relay 2** (mis. lampu taman). Judul kartu
  otomatis mengikuti nama relay yang sudah kamu edit.
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
- `POST /api/data` — kirim `{"device":"...", ...field yang mau diubah}`. Bisa kirim
  cuma sebagian field (misal `{"device":"ESP01_01","status3":"ON"}`) — field lain
  otomatis dipertahankan dari data terakhir device itu. Setelah tersimpan, backend
  otomatis publish ke MQTT topic `smarthome/<device>/status`. **Tidak memengaruhi**
  status online/offline device.
- `POST /api/heartbeat` — KHUSUS dipanggil ESP8266 tiap 15 detik, body
  `{"device":"...", "suhu":"..", "suhu2":"..", "suhu3":".."}`. Suhu di sini opsional
  (kalau tidak dikirim, nilai lama dipertahankan). Field ini cuma nyentuh kolom suhu
  & `last_heartbeat`, dipakai buat hitung `status_device` — TIDAK memengaruhi relay.
- `GET /api/data?limit=50` — data terbaru semua device (semua kolom)
- `GET /api/data/:device?limit=50` — data terbaru satu device (semua kolom)
- `GET /api/status` — baris terakhir tiap device (semua kolom)
- `GET /api/status/:device` — baris terakhir satu device (semua kolom)

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
3. Install 2 library lewat Library Manager:
   - **PubSubClient** (by Nick O'Leary)
   - **ArduinoJson** (by Benoit Blanchon)
4. Edit di bagian atas file:
   - `WIFI_SSID` / `WIFI_PASSWORD` → sesuai WiFi kamu (perlu akses internet)
   - `MQTT_HOST`, `MQTT_PORT`, `MQTT_USER`, `MQTT_PASS` → sama seperti yang diisi di
     `.env` backend
   - `DEVICE_NAME` → harus SAMA PERSIS dengan device yang dipilih di app Android
5. Pilih board yang sesuai (Tools → Board → mis. "Generic ESP8266 Module").
6. Klik Upload.
7. Buka Serial Monitor (baud rate 115200) — harus muncul "WiFi terhubung" lalu
   "Terhubung" (ke HiveMQ) dan "Subscribe ke: smarthome/ESP01_01/status".

## 5. Cek data masuk

Buka `https://<url-vercel-kamu>/api/data` di browser — harus muncul data terbaru.
Tekan tombol ON/OFF di app Android, LED ESP8266 harus langsung berubah dalam
waktu singkat (bukan nunggu 3 detik lagi).

## 5. App Android "Smart Home IoT"

App Android di folder `android/` punya:
- Header "MQTT - Neon PostgreSQL - Vercel - Android"
- Kartu **Status Database** (koneksi app ke backend Vercel/Neon)
- Kartu **Status Device** — dihitung backend dari heartbeat terakhir ESP8266 (field
  `status_device` di response API): kalau lebih dari 35 detik tanpa heartbeat, otomatis
  "Offline". Dibaca lewat REST API biasa, sama seperti suhu & relay -- app tidak perlu
  simpan kredensial broker MQTT sama sekali.
- Combo box pilih device
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
