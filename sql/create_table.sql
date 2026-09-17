-- Jalankan ini di Neon Console -> SQL Editor (atau lewat tab "Tables" -> New table)

CREATE TABLE IF NOT EXISTS iot2 (
    id     SERIAL PRIMARY KEY,
    device VARCHAR(50)  NOT NULL,
    status VARCHAR(20)  NOT NULL,
    time   TIMESTAMPTZ  NOT NULL DEFAULT NOW()
);

-- (opsional) index supaya query per-device lebih cepat kalau datanya sudah banyak
CREATE INDEX IF NOT EXISTS idx_iot2_device_time ON iot2 (device, time DESC);
