# ESP8266 → HTTPS → Vercel API → Neon PostgreSQL

Project terpisah dari "IoT Industrial System". Alurnya kebalikan dari project sebelumnya:
di sini **ESP8266 yang mengirim data** ke server, bukan cuma dibaca.

```
[ESP8266] --HTTPS POST--> [Vercel API] --INSERT--> [Neon PostgreSQL: iot2]
                                 |
                            GET /api/data
                                 |
                        (app/dashboard kamu nanti)
```

Tabel `iot2`:

| kolom    | keterangan                          |
|----------|--------------------------------------|
| `id`     | auto increment                       |
| `device` | nama/ID alat ESP8266                 |
| `status` | contoh: "ON" / "OFF"                 |
| `time`   | otomatis diisi server saat data masuk |

## Struktur folder

```
esp8266-iot2/
├── sql/                 -> script bikin tabel iot2 di Neon
├── backend/             -> REST API (Node.js + Express), deploy ke Vercel
└── esp8266-firmware/    -> kode Arduino (.ino) untuk ESP8266
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

Isi `DATABASE_URL` di `.env` dengan connection string Neon (pooled connection).

Tes lokal dulu:
```bash
npm start
```

Tes kirim data manual pakai curl (atau Postman):
```bash
curl -X POST http://localhost:3000/api/data ^
  -H "Content-Type: application/json" ^
  -d "{\"device\":\"ESP8266_01\",\"status\":\"ON\"}"
```

Kalau sudah oke, deploy ke Vercel (Add New Project → pilih repo ini → Root Directory: `backend`
→ tambahkan `DATABASE_URL` di Environment Variables → Deploy). Setelah dapat URL-nya
(mis. `https://esp8266-iot2-backend.vercel.app`), itu yang dipakai di firmware ESP8266.

### Endpoint yang tersedia
- `POST /api/data` — dipanggil ESP8266, body: `{"device": "...", "status": "..."}`
- `GET /api/data?limit=50` — data terbaru semua device
- `GET /api/data/:device?limit=50` — data terbaru satu device
- `GET /api/status` — status terakhir tiap device

## 3. Upload firmware ke ESP8266

1. Buka `esp8266-firmware/esp8266_iot2.ino` di Arduino IDE.
2. Install board package **ESP8266** (kalau belum): File → Preferences → Additional Board
   Manager URLs → tambahkan `http://arduino.esp8266.com/stable/package_esp8266com_index.json`
   → Tools → Board → Boards Manager → cari "esp8266" → Install.
3. Install library **ArduinoJson** lewat Library Manager (Sketch → Include Library →
   Manage Libraries → cari "ArduinoJson" by Benoit Blanchon → Install).
4. Edit di bagian atas file:
   - `WIFI_SSID` dan `WIFI_PASSWORD` → sesuai WiFi kamu
   - `API_URL` → URL Vercel backend kamu + `/api/data`
   - `DEVICE_NAME` → nama unik untuk alat ini
   - `STATUS_PIN` → pin yang mau dibaca (sesuaikan dengan wiring kamu)
5. Pilih board yang sesuai (Tools → Board → sesuai modul ESP8266 kamu, mis. "NodeMCU 1.0").
6. Klik Upload.
7. Buka Serial Monitor (baud rate 115200) untuk lihat log koneksi WiFi & pengiriman data.

## 4. Cek data masuk

Buka `https://<url-vercel-kamu>/api/data` di browser — harus muncul data yang dikirim ESP8266.

---

## Catatan

- Endpoint ini **belum pakai autentikasi** (sesuai permintaan, biar simpel dulu). Siapa pun yang
  tahu URL-nya bisa kirim data. Kalau nanti mau ditambah keamanan, saya bisa bantu tambahkan
  API key sederhana.
- `client.setInsecure()` di kode ESP8266 melewati verifikasi sertifikat HTTPS — umum dipakai
  untuk project hobi karena ESP8266 berat kalau harus simpan root CA lengkap.
