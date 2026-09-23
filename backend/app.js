const express = require('express');
const cors = require('cors');
const { Pool } = require('pg');
const mqtt = require('mqtt');

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
const ALL_FIELDS = [...STATUS_FIELDS, ...SUHU_FIELDS];

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
app.post('/api/data', async (req, res) => {
  try {
    const { device } = req.body;
    if (!device) {
      return res.status(400).json({ error: 'Field "device" wajib diisi' });
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
        merged[field] = field.startsWith('suhu') ? '0' : 'OFF';
      }
    }

    const insertResult = await pool.query(
      `INSERT INTO iot2 (device, status, status2, status3, status4, status5, suhu, suhu2, suhu3, time)
       VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, NOW())
       ON CONFLICT (device) DO UPDATE SET
         status  = EXCLUDED.status,
         status2 = EXCLUDED.status2,
         status3 = EXCLUDED.status3,
         status4 = EXCLUDED.status4,
         status5 = EXCLUDED.status5,
         suhu    = EXCLUDED.suhu,
         suhu2   = EXCLUDED.suhu2,
         suhu3   = EXCLUDED.suhu3,
         time    = EXCLUDED.time
       RETURNING *`,
      [
        device,
        merged.status, merged.status2, merged.status3, merged.status4, merged.status5,
        merged.suhu, merged.suhu2, merged.suhu3,
      ]
    );

    const savedRow = insertResult.rows[0];

    // Publish ke HiveMQ (best-effort, tidak memblokir response kalau gagal)
    await publishUpdate(device, savedRow);

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
    const { device, suhu, suhu2, suhu3, cuaca } = req.body;
    if (!device) {
      return res.status(400).json({ error: 'Field "device" wajib diisi' });
    }

    const result = await pool.query(
      `INSERT INTO iot2 (device, suhu, suhu2, suhu3, cuaca, last_heartbeat)
       VALUES ($1, $2, $3, $4, $5, NOW())
       ON CONFLICT (device) DO UPDATE SET
         suhu = COALESCE(EXCLUDED.suhu, iot2.suhu),
         suhu2 = COALESCE(EXCLUDED.suhu2, iot2.suhu2),
         suhu3 = COALESCE(EXCLUDED.suhu3, iot2.suhu3),
         cuaca = COALESCE(EXCLUDED.cuaca, iot2.cuaca),
         last_heartbeat = NOW()
       RETURNING *`,
      [
        device,
        suhu !== undefined ? String(suhu) : null,
        suhu2 !== undefined ? String(suhu2) : null,
        suhu3 !== undefined ? String(suhu3) : null,
        cuaca !== undefined ? String(cuaca) : null,
      ]
    );

    res.json({ success: true, data: withDeviceStatus(result.rows[0]) });
  } catch (err) {
    console.error('Error in POST /api/heartbeat:', err);
    res.status(500).json({ error: 'Gagal menyimpan heartbeat' });
  }
});

// GET /api/data?limit=50 -> data terbaru dari semua device
app.get('/api/data', async (req, res) => {
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
app.get('/api/data/:device', async (req, res) => {
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
app.get('/api/status', async (req, res) => {
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
app.get('/api/status/:device', async (req, res) => {
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
