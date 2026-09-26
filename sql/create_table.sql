-- Jalankan ini di Neon Console -> SQL Editor (atau lewat tab "Tables" -> New table)

CREATE TABLE IF NOT EXISTS iot2 (
    id     SERIAL PRIMARY KEY,
    device VARCHAR(50)  NOT NULL,
    status VARCHAR(20)  NOT NULL,
    time   TIMESTAMPTZ  NOT NULL DEFAULT NOW()
);

-- (opsional) index supaya query per-device lebih cepat kalau datanya sudah banyak
CREATE INDEX IF NOT EXISTS idx_iot2_device_time ON iot2 (device, time DESC);

-- Menambah kolom baru ke tabel iot2 yang SUDAH ADA (dijalankan belakangan)
-- Pakai DEFAULT supaya baris data lama yang sudah ada tidak error (karena NOT NULL).
ALTER TABLE iot2
  ADD COLUMN IF NOT EXISTS status2 VARCHAR(20) NOT NULL DEFAULT 'OFF',
  ADD COLUMN IF NOT EXISTS status3 VARCHAR(20) NOT NULL DEFAULT 'OFF',
  ADD COLUMN IF NOT EXISTS status4 VARCHAR(20) NOT NULL DEFAULT 'OFF',
  ADD COLUMN IF NOT EXISTS status5 VARCHAR(20) NOT NULL DEFAULT 'OFF',
  ADD COLUMN IF NOT EXISTS suhu    VARCHAR(20) NOT NULL DEFAULT '0',
  ADD COLUMN IF NOT EXISTS suhu2   VARCHAR(20) NOT NULL DEFAULT '0',
  ADD COLUMN IF NOT EXISTS suhu3   VARCHAR(20) NOT NULL DEFAULT '0';

-- ============================================================
-- Migrasi: dari "1 baris per kejadian" jadi "1 baris per device"
-- ============================================================

-- 1. Buang baris-baris lama, sisakan cuma yang TERBARU per device
DELETE FROM iot2
WHERE id NOT IN (
  SELECT DISTINCT ON (device) id
  FROM iot2
  ORDER BY device, time DESC, id DESC
);

-- 2. Jadikan device unik, supaya bisa di-upsert (insert-atau-update)
ALTER TABLE iot2 ADD CONSTRAINT iot2_device_unique UNIQUE (device);

-- Perbaikan bug: pisahkan "waktu heartbeat asli dari ESP" dari "waktu baris berubah"
-- (sebelumnya kolom 'time' dipakai untuk dua-duanya, jadi update dari APP ikut
-- bikin status_device keliatan online padahal ESP-nya mati).
ALTER TABLE iot2 ADD COLUMN IF NOT EXISTS last_heartbeat TIMESTAMPTZ;

-- Jaga-jaga: kolom "status" (relay 1) awalnya NOT NULL tanpa default. Kasih default
-- juga, supaya heartbeat pertama dari device BARU (yang belum pernah kontrol relay
-- sama sekali) tidak gagal karena constraint NOT NULL.
ALTER TABLE iot2 ALTER COLUMN status SET DEFAULT 'OFF';

-- Fix bug: heartbeat gagal (500) karena kolom "status" belum punya default value
ALTER TABLE iot2 ALTER COLUMN status SET DEFAULT 'OFF';

-- Diagnostik: cek apakah constraint UNIQUE pada device beneran ada
SELECT conname FROM pg_constraint WHERE conrelid = 'iot2'::regclass AND contype = 'u';

-- Tambahan: kolom status cuaca (hujan/cerah/mendung). Data suhu1,2,3 TIDAK nambah
-- kolom baru -- kelembaban digabung jadi satu teks di kolom suhu yang sudah ada
-- (contoh isi: "28.5C, 65%RH").
ALTER TABLE iot2 ADD COLUMN IF NOT EXISTS cuaca VARCHAR(20) DEFAULT 'Cerah';

-- ============================================================
-- Fitur AUTO (jadwal nyala/mati lampu otomatis)
-- Format isi kolom:  "<ON|OFF>,<jam_nyala>,<jam_mati>"  (24 jam, WIB)
--   contoh: "ON,18:00,06:00"  -> aktif, nyala 18:00, mati 06:00 (lewat tengah malam OK)
--           "OFF,18:00,06:00" -> jadwal dimatikan (jam tetap tersimpan)
-- auto1 -> mengatur Relay 1 (status)   | auto2 -> mengatur Relay 2 (status2)
-- ============================================================
ALTER TABLE iot2
  ADD COLUMN IF NOT EXISTS auto1 VARCHAR(20) NOT NULL DEFAULT 'OFF,18:00,06:00',
  ADD COLUMN IF NOT EXISTS auto2 VARCHAR(20) NOT NULL DEFAULT 'OFF,18:00,06:00';

-- ============================================================
-- Fitur SENSOR: PIR (gerak) & Pintu/Jendela
-- Ini BUKAN relay -- tidak ada tombol ON/OFF dari app, cuma laporan dari ESP (lewat
-- POST /api/heartbeat). status5 (relay ke-5, TIDAK DIPAKAI LAGI) dibiarkan apa adanya
-- di database (aman, tidak mengganggu), cukup diabaikan oleh backend & app.
-- ============================================================
ALTER TABLE iot2
  ADD COLUMN IF NOT EXISTS status_pir     VARCHAR(20) NOT NULL DEFAULT 'OFF', -- ON = ada gerak, OFF = aman
  ADD COLUMN IF NOT EXISTS status_dor_win VARCHAR(20) NOT NULL DEFAULT 'OFF'; -- ON = buka, OFF = tutup
