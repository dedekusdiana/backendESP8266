const express = require('express');
const cors = require('cors');
const { Pool } = require('pg');

const app = express();
app.use(cors());
app.use(express.json());

const pool = new Pool({
  connectionString: process.env.DATABASE_URL,
  ssl: { rejectUnauthorized: false },
});

// Kolom relay (ON/OFF) dan sensor suhu yang didukung tabel iot2.
const STATUS_FIELDS = ['status', 'status2', 'status3', 'status4', 'status5'];
const SUHU_FIELDS = ['suhu', 'suhu2', 'suhu3'];
const ALL_FIELDS = [...STATUS_FIELDS, ...SUHU_FIELDS];

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
      `INSERT INTO iot2 (device, status, status2, status3, status4, status5, suhu, suhu2, suhu3)
       VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9)
       RETURNING *`,
      [
        device,
        merged.status, merged.status2, merged.status3, merged.status4, merged.status5,
        merged.suhu, merged.suhu2, merged.suhu3,
      ]
    );

    res.status(201).json({ success: true, data: insertResult.rows[0] });
  } catch (err) {
    console.error('Error in POST /api/data:', err);
    res.status(500).json({ error: 'Gagal menyimpan data' });
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
    res.json({ devices: rows });
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

    res.json(rows[0]);
  } catch (err) {
    console.error('Error in GET /api/status/:device:', err);
    res.status(500).json({ error: 'Gagal mengambil status device' });
  }
});

module.exports = app;
