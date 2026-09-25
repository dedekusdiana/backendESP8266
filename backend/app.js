const express = require('express');
const cors = require('cors');
const { Pool } = require('pg');
const mqtt = require('mqtt');
const { codeMatches, requireDeviceCode, requireAdmin } = require('./auth');
const { notifyDevice } = require('./push');

const app = express();
app.use(cors());
app.use(express.json());

const pool = new Pool({
  connectionString: process.env.DATABASE_URL,
  ssl: { rejectUnauthorized: false },
});

// Kalau sudah lebih dari sekian detik tanpa kabar (heartbeat) dari device,
// dianggap OFFLINE. Heartbeat ESP8266 dikirim tiap 15 detik, threshold 35 detik
// (lebih longgar dari interval heartbeat) supaya tidak "kedip" karena jitter jaringan.
const OFFLINE_THRESHOLD_SECONDS = Number(process.env.OFFLINE_THRESHOLD_SECONDS || 35);

function withDeviceStatus(row) {
  if (!row) return row;
  const heartbeatTime = row.last_heartbeat || null;
  const secondsSinceHeartbeat = heartbeatTime
    ? (Date.now() - new Date(heartbeatTime).getTime()) / 1000
    : Infinity; // belum pernah heartbeat sama sekali -> anggap offline
  return {
    ...row,
    status_device: secondsSinceHeartbeat < OFFLINE_THRESHOLD_SECONDS ? 'online' : 'offline',
  };
}

// Kolom relay (ON/OFF) dan sensor suhu yang didukung tabel iot2.
const STATUS_FIELDS = ['status', 'status2', 'status3', 'status4', 'status5'];
const SUHU_FIELDS = ['suhu', 'suhu2', 'suhu3'];
// Jadwal otomatis lampu. Format: "<ON|OFF>,<HH:MM nyala>,<HH:MM mati>", contoh "ON,18:00,06:00".
// auto1 mengatur Relay 1 (status), auto2 mengatur Relay 2 (status2) -- pemetaan ada di firmware.
const AUTO_FIELDS = ['auto1', 'auto2'];
const RELAY_LABELS = { status: 'Relay 1', status2: 'Relay 2', status3: 'Relay 3', status4: 'Relay 4', status5: 'Relay 5' };
const AUTO_DEFAULT = 'OFF,18:00,06:00';
const AUTO_REGEX = /^(ON|OFF),([01]\d|2[0-3]):[0-5]\d,([01]\d|2[0-3]):[0-5]\d$/;
const ALL_FIELDS = [...STATUS_FIELDS, ...SUHU_FIELDS, ...AUTO_FIELDS];

// Payload MQTT sengaja dibuat ringkas (hanya yang dibutuhkan ESP), supaya muat di buffer ESP8266.
function mqttPayload(row) {
  const payload = { device: row.device };
  for (const f of [...STATUS_FIELDS, ...AUTO_FIELDS]) payload[f] = row[f];
  return payload;
}

// ---------------------------------------------------------------
// Publish ke HiveMQ setiap ada update, supaya ESP8266 yang subscribe
// dapat notifikasi instan (bukan polling). Best-effort: kalau broker
// lagi bermasalah, ini tidak menggagalkan response API ke Android.
// ---------------------------------------------------------------
async function publishUpdate(device, payload) {
  if (!process.env.MQTT_HOST) return; // belum dikonfigurasi, skip diam-diam

  const url = `mqtts://${process.env.MQTT_HOST}:${process.env.MQTT_PORT || 8883}`;

  return new Promise((resolve) => {
    const client = mqtt.connect(url, {
      username: process.env.MQTT_USERNAME,
      password: process.env.MQTT_PASSWORD,
      connectTimeout: 5000,
      reconnectPeriod: 0, // jangan auto-reconnect, ini cuma sekali pakai
    });

    const finish = () => {
      client.end(true);
      resolve();
    };

    client.on('connect', () => {
      const topic = `smarthome/${device}/status`;
      // retain: true -- broker simpan pesan ini, dikirim OTOMATIS ke subscriber baru
      // (termasuk ESP yang baru nyala lagi setelah mati/putus koneksi).
      client.publish(topic, JSON.stringify(payload), { qos: 1, retain: true }, () => finish());
    });

    client.on('error', (err) => {
      console.error('MQTT publish error:', err.message);
      finish();
    });

    // Jaga-jaga kalau connect tidak pernah selesai
    setTimeout(finish, 6000);
  });
}

// Health check
app.get('/', (req, res) => {
  res.json({ status: 'ok', service: 'esp8266-iot2-backend' });
});

// ---------------------------------------------------------------
// POST /api/data
// Body JSON: { "device": "ESP01_01", "status": "ON" }  <- bisa kirim sebagian field saja
// Field yang tidak dikirim akan otomatis diambil dari data terakhir device itu
// (jadi app Android bisa update 1 relay saja tanpa perlu tahu status relay lain).
// Field yang belum pernah ada sama sekali -> default 'OFF' (status) / '0' (suhu).
// ---------------------------------------------------------------
app.post('/api/data', requireDeviceCode, async (req, res) => {
  try {
    const { device } = req.body;
    if (!device) {
      return res.status(400).json({ error: 'Field "device" wajib diisi' });
    }

    // Validasi format jadwal auto (kalau dikirim)
    for (const f of AUTO_FIELDS) {
      if (req.body[f] !== undefined && !AUTO_REGEX.test(String(req.body[f]))) {
        return res.status(400).json({
          error: `Field \"${f}\" harus berformat \"ON,HH:MM,HH:MM\" atau \"OFF,HH:MM,HH:MM\"`,
        });
      }
    }

    const latestResult = await pool.query(
      `SELECT * FROM iot2 WHERE device = $1 ORDER BY time DESC LIMIT 1`,
      [device]
    );
    const latest = latestResult.rows[0] || {};

    const merged = {};
    for (const field of ALL_FIELDS) {
      if (req.body[field] !== undefined) {
        merged[field] = String(req.body[field]);
      } else if (latest[field] !== undefined) {
        merged[field] = latest[field];
      } else {
        merged[field] = field.startsWith('suhu') ? '0' : field.startsWith('auto') ? AUTO_DEFAULT : 'OFF';
      }
    }

    const insertResult = await pool.query(
      `INSERT INTO iot2 (device, status, status2, status3, status4, status5, suhu, suhu2, suhu3, auto1, auto2, time)
       VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, NOW())
       ON CONFLICT (device) DO UPDATE SET
         status  = EXCLUDED.status,
         status2 = EXCLUDED.status2,
         status3 = EXCLUDED.status3,
         status4 = EXCLUDED.status4,
         status5 = EXCLUDED.status5,
         suhu    = EXCLUDED.suhu,
         suhu2   = EXCLUDED.suhu2,
         suhu3   = EXCLUDED.suhu3,
         auto1   = EXCLUDED.auto1,
         auto2   = EXCLUDED.auto2,
         time    = EXCLUDED.time
       RETURNING *`,
      [
        device,
        merged.status, merged.status2, merged.status3, merged.status4, merged.status5,
        merged.suhu, merged.suhu2, merged.suhu3,
        merged.auto1, merged.auto2,
      ]
    );

    const savedRow = insertResult.rows[0];

    // Publish ke HiveMQ (best-effort, tidak memblokir response kalau gagal)
    await publishUpdate(device, mqttPayload(savedRow));

    // Notifikasi push kalau ada relay yang benar-benar berubah (best-effort, tidak menunggu)
    for (const field of STATUS_FIELDS) {
      if (latest[field] !== undefined && latest[field] !== merged[field]) {
        const label = RELAY_LABELS[field] || field;
        notifyDevice(device, label, `${label} ${merged[field] === 'ON' ? 'menyala' : 'mati'}`);
      }
    }

    res.status(201).json({ success: true, data: withDeviceStatus(savedRow) });
  } catch (err) {
    console.error('Error in POST /api/data:', err);
    res.status(500).json({ error: 'Gagal menyimpan data' });
  }
});

// ---------------------------------------------------------------
// POST /api/heartbeat
// Dipanggil KHUSUS oleh ESP8266 tiap 15 detik, TERPISAH dari /api/data (kontrol relay).
// Cuma nyentuh kolom last_heartbeat -- supaya update relay dari APP tidak ikut
// bikin status_device keliatan online.
// ---------------------------------------------------------------
app.post('/api/heartbeat', async (req, res) => {
  try {
    const { device, suhu, suhu2, suhu3, cuaca, status, status2 } = req.body;
    if (!device) {
      return res.status(400).json({ error: 'Field "device" wajib diisi' });
    }

    // status / status2 opsional: dipakai ESP melapor kondisi relay setelah jadwal auto berjalan
    // (tidak lewat /api/data lagi, karena /api/data sekarang butuh kode aktivasi milik pelanggan).
    const onOff = (v) => (v === 'ON' || v === 'OFF' ? v : null);

    const before = (await pool.query(`SELECT status, status2, notified_offline FROM iot2 WHERE device = $1`, [device])).rows[0] || {};

    const result = await pool.query(
      `INSERT INTO iot2 (device, status, status2, suhu, suhu2, suhu3, cuaca, last_heartbeat, notified_offline)
       VALUES ($1, COALESCE($2::varchar, 'OFF'), COALESCE($3::varchar, 'OFF'), $4, $5, $6, $7, NOW(), FALSE)
       ON CONFLICT (device) DO UPDATE SET
         status  = COALESCE($2::varchar, iot2.status),
         status2 = COALESCE($3::varchar, iot2.status2),
         suhu = COALESCE(EXCLUDED.suhu, iot2.suhu),
         suhu2 = COALESCE(EXCLUDED.suhu2, iot2.suhu2),
         suhu3 = COALESCE(EXCLUDED.suhu3, iot2.suhu3),
         cuaca = COALESCE(EXCLUDED.cuaca, iot2.cuaca),
         last_heartbeat = NOW(),
         notified_offline = FALSE
       RETURNING *`,
      [
        device,
        onOff(status),
        onOff(status2),
        suhu !== undefined ? String(suhu) : null,
        suhu2 !== undefined ? String(suhu2) : null,
        suhu3 !== undefined ? String(suhu3) : null,
        cuaca !== undefined ? String(cuaca) : null,
      ]
    );
    const savedRow = result.rows[0];

    // Notifikasi kalau relay berubah gara-gara jadwal auto (before ada isinya hanya kalau device sudah pernah lapor)
    if (before.status !== undefined) {
      for (const field of ['status', 'status2']) {
        if (before[field] !== undefined && before[field] !== savedRow[field]) {
          const label = RELAY_LABELS[field] || field;
          notifyDevice(device, label, `${label} ${savedRow[field] === 'ON' ? 'menyala' : 'mati'} (jadwal auto)`);
        }
      }
    }
    // Alat baru online lagi setelah sempat offline -> kabari (notified_offline sebelumnya true)
    if (before.notified_offline) {
      notifyDevice(device, 'Kembali online', `${device} terhubung kembali`);
    }

    // Tidak mengembalikan data device ke pemanggil (endpoint ini tanpa kode aktivasi)
    res.json({ success: true, status_device: withDeviceStatus(savedRow).status_device });
  } catch (err) {
    console.error('Error in POST /api/heartbeat:', err);
    res.status(500).json({ error: 'Gagal menyimpan heartbeat' });
  }
});

// ---------------------------------------------------------------
// GET /api/cron/check-offline -- dipanggil terjadwal (Vercel Cron, lihat vercel.json) tiap 5 menit.
// Device yang sudah lewat OFFLINE_THRESHOLD_SECONDS dan belum pernah dikabari -> kirim notifikasi
// sekali saja (ditandai notified_offline), supaya tidak spam tiap kali cron jalan.
// ---------------------------------------------------------------
app.get('/api/cron/check-offline', async (req, res) => {
  try {
    const { rows } = await pool.query(
      `UPDATE iot2 SET notified_offline = TRUE
       WHERE notified_offline = FALSE
         AND last_heartbeat < NOW() - INTERVAL '1 second' * $1
       RETURNING device`,
      [OFFLINE_THRESHOLD_SECONDS]
    );
    for (const row of rows) {
      await notifyDevice(row.device, 'Device offline', `${row.device} tidak terhubung`);
    }
    res.json({ success: true, notified: rows.map(r => r.device) });
  } catch (err) {
    console.error('Error in GET /api/cron/check-offline:', err);
    res.status(500).json({ error: 'Gagal cek device offline' });
  }
});

// ---------------------------------------------------------------
// POST /api/claim  -- app "menambahkan" device milik pelanggan.
// Body: { "device": "ESP-A1B2C3", "code": "ABCDE-FGHJK" } (kode dari stiker produk)
// Kode salah -> 403. Kode benar -> 200 (data = null kalau device belum pernah online).
// ---------------------------------------------------------------
app.post('/api/claim', async (req, res) => {
  try {
    const { device, code } = req.body;
    if (!device || !code) {
      return res.status(400).json({ error: 'Field "device" dan "code" wajib diisi' });
    }
    if (!codeMatches(device, code)) {
      return res.status(403).json({ error: 'ID device atau kode aktivasi salah' });
    }
    const { rows } = await pool.query(`SELECT * FROM iot2 WHERE device = $1 LIMIT 1`, [device]);
    res.json({ success: true, data: rows[0] ? withDeviceStatus(rows[0]) : null });
  } catch (err) {
    console.error('Error in POST /api/claim:', err);
    res.status(500).json({ error: 'Gagal memverifikasi device' });
  }
});

// GET /api/data?limit=50 -> data terbaru dari semua device
app.get('/api/data', requireAdmin, async (req, res) => {
  try {
    const limit = Math.min(Number(req.query.limit) || 50, 500);
    const { rows } = await pool.query(
      `SELECT * FROM iot2 ORDER BY time DESC LIMIT $1`,
      [limit]
    );
    res.json({ data: rows });
  } catch (err) {
    console.error('Error in GET /api/data:', err);
    res.status(500).json({ error: 'Gagal mengambil data' });
  }
});

// GET /api/data/:device?limit=50 -> data terbaru untuk satu device
app.get('/api/data/:device', requireDeviceCode, async (req, res) => {
  try {
    const { device } = req.params;
    const limit = Math.min(Number(req.query.limit) || 50, 500);
    const { rows } = await pool.query(
      `SELECT * FROM iot2 WHERE device = $1 ORDER BY time DESC LIMIT $2`,
      [device, limit]
    );
    res.json({ device, data: rows });
  } catch (err) {
    console.error('Error in GET /api/data/:device:', err);
    res.status(500).json({ error: 'Gagal mengambil data' });
  }
});

// GET /api/status -> baris terakhir untuk setiap device (semua kolom)
app.get('/api/status', requireAdmin, async (req, res) => {
  try {
    const query = `
      SELECT DISTINCT ON (device) *
      FROM iot2
      ORDER BY device, time DESC
    `;
    const { rows } = await pool.query(query);
    res.json({ devices: rows.map(withDeviceStatus) });
  } catch (err) {
    console.error('Error in GET /api/status:', err);
    res.status(500).json({ error: 'Gagal mengambil status' });
  }
});

// GET /api/status/:device -> baris terakhir untuk SATU device (semua kolom)
app.get('/api/status/:device', requireDeviceCode, async (req, res) => {
  try {
    const { device } = req.params;
    const { rows } = await pool.query(
      `SELECT * FROM iot2 WHERE device = $1 ORDER BY time DESC LIMIT 1`,
      [device]
    );

    if (rows.length === 0) {
      return res.status(404).json({ error: `Belum ada data untuk device "${device}"` });
    }

    res.json(withDeviceStatus(rows[0]));
  } catch (err) {
    console.error('Error in GET /api/status/:device:', err);
    res.status(500).json({ error: 'Gagal mengambil status device' });
  }
});

module.exports = app;


