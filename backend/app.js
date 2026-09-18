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

// Health check
app.get('/', (req, res) => {
  res.json({ status: 'ok', service: 'esp8266-iot2-backend' });
});

// ---------------------------------------------------------------
// POST /api/data
// Dipanggil oleh ESP8266. Body JSON: { "device": "ESP8266_01", "status": "ON" }
// Kolom "time" diisi otomatis oleh server (NOW()) supaya ESP8266 tidak perlu
// punya jam/RTC sendiri. Kalau body menyertakan "time" (format ISO8601),
// nilai itu yang dipakai sebagai gantinya.
// ---------------------------------------------------------------
app.post('/api/data', async (req, res) => {
  try {
    const { device, status, time } = req.body;

    if (!device || !status) {
      return res.status(400).json({ error: 'Field "device" dan "status" wajib diisi' });
    }

    const query = time
      ? `INSERT INTO iot2 (device, status, time) VALUES ($1, $2, $3) RETURNING *`
      : `INSERT INTO iot2 (device, status) VALUES ($1, $2) RETURNING *`;
    const params = time ? [device, status, time] : [device, status];

    const { rows } = await pool.query(query, params);

    res.status(201).json({ success: true, data: rows[0] });
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
      `SELECT id, device, status, time FROM iot2 ORDER BY time DESC LIMIT $1`,
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
      `SELECT id, device, status, time FROM iot2 WHERE device = $1 ORDER BY time DESC LIMIT $2`,
      [device, limit]
    );
    res.json({ device, data: rows });
  } catch (err) {
    console.error('Error in GET /api/data/:device:', err);
    res.status(500).json({ error: 'Gagal mengambil data' });
  }
});

// GET /api/status -> status terkini (baris terakhir) untuk setiap device
app.get('/api/status', async (req, res) => {
  try {
    const query = `
      SELECT DISTINCT ON (device) device, status, time
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

// GET /api/status/:device -> status terkini untuk SATU device (dipakai ESP8266 buat polling LED)
app.get('/api/status/:device', async (req, res) => {
  try {
    const { device } = req.params;
    const { rows } = await pool.query(
      `SELECT device, status, time FROM iot2 WHERE device = $1 ORDER BY time DESC LIMIT 1`,
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
