const express = require('express');
const cors = require('cors');
const { Pool } = require('pg');
const mqtt = require('mqtt');
const crypto = require('crypto');
const { codeMatches, requireDeviceCode, requireAdmin } = require('./auth');
const { sendTelegram, formatStatusMessage } = require('./telegram');
const { notifyDevice } = require('./push');

const app = express();
app.use(cors());
app.use(express.json());

const pool = new Pool({
  connectionString: process.env.DATABASE_URL,
  ssl: { rejectUnauthorized: false },
});

// Status online/offline TIDAK lagi dihitung dari heartbeat. ESP memakai MQTT Last Will (LWT) di topic
// smarthome/<device>/online ("online" saat konek, "offline" dipublish BROKER saat ESP putus).
// EMQX Rule Engine meneruskan perubahan itu ke POST /api/mqtt-event, yang menyimpan kolom is_online.
// last_heartbeat sekarang berarti "kontak terakhir" (waktu event online/offline/telemetry terakhir).
function withDeviceStatus(row) {
  if (!row) return row;
  return {
    ...row,
    status_device: row.is_online ? 'online' : 'offline',
  };
}

// Kolom relay (ON/OFF) dan sensor suhu yang didukung tabel iot2.
const STATUS_FIELDS = ['status', 'status2', 'status3', 'status4', 'status5']; // status5 = relay output PIR (ESP32)
// Dinotifikasi ke Telegram cuma relay yang dinyalakan manusia -- status5 (output PIR) SENGAJA
// dikecualikan, karena bisa berubah tiap ada gerak dan akan membanjiri chat dengan notifikasi.
const NOTIFY_RELAY_FIELDS = ['status', 'status2', 'status3', 'status4'];
const RELAY_LABELS = { status: 'Relay 1', status2: 'Relay 2', status3: 'Relay 3', status4: 'Relay 4' };

// Daftar device milik pemilik bot (TELEGRAM_DEVICE_IDS, pisah koma). Dipakai untuk membatasi
// notifikasi relay DAN perintah "cek rumah" ke device ini saja. Kosong = semua device ikut.
function ownTelegramDevices() {
  return String(process.env.TELEGRAM_DEVICE_IDS || '')
    .split(',').map((s) => s.trim()).filter(Boolean);
}
function isOwnTelegramDevice(device) {
  const own = ownTelegramDevices();
  return own.length === 0 || own.includes(device);
}

// Kirim notifikasi perubahan relay:
//  - Push FCM ke semua HP yang menambahkan device ini (topic per device) -> untuk pelanggan.
//  - Telegram hanya untuk device milik pemilik (TELEGRAM_DEVICE_IDS).
// Dua-duanya best-effort (tidak pernah melempar error). Harus di-await di pemanggil:
// di Vercel (serverless) proses bisa dibekukan begitu response dikirim.
function notifyRelayChange(device, field, isOn, viaAuto) {
  const label = RELAY_LABELS[field] || field;
  const verb = isOn ? 'menyala' : 'mati';
  const tasks = [notifyDevice(device, device, `${label} ${verb}${viaAuto ? ' (jadwal auto)' : ''}`)];
  if (isOwnTelegramDevice(device)) {
    tasks.push(sendTelegram(`${label} ${verb} ${isOn ? '🟢' : '⚪'} (${viaAuto ? 'jadwal auto, ' : ''}${device})`));
  }
  return Promise.all(tasks);
}
// Sensor (bukan relay -- tidak bisa dinyalakan/dimatikan dari app, cuma laporan dari ESP lewat /api/heartbeat)
const SENSOR_FIELDS = ['status_pir', 'status_dor_win'];
const SUHU_FIELDS = ['suhu', 'suhu2', 'suhu3'];
// Jadwal otomatis lampu. Format: "<ON|OFF>,<HH:MM nyala>,<HH:MM mati>", contoh "ON,18:00,06:00".
// auto1 mengatur Relay 1 (status), auto2 mengatur Relay 2 (status2) -- pemetaan ada di firmware.
const AUTO_FIELDS = ['auto1', 'auto2'];
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

    // Notifikasi (push + Telegram) kalau ada relay (manual, bukan output PIR) yang benar-benar berubah
    for (const field of NOTIFY_RELAY_FIELDS) {
      if (latest[field] !== undefined && latest[field] !== merged[field]) {
        await notifyRelayChange(device, field, merged[field] === 'ON', false);
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
// SEKARANG hanya untuk laporan relay dari ESP (saat jadwal auto mengubah relay) -- BUKAN heartbeat lagi.
// TIDAK menyentuh last_heartbeat / is_online: status online/offline datang dari MQTT LWT
// lewat POST /api/mqtt-event. (Nama endpoint dipertahankan supaya firmware lama tetap kompatibel.)
// ---------------------------------------------------------------
app.post('/api/heartbeat', async (req, res) => {
  try {
    const { device, suhu, suhu2, suhu3, cuaca, status, status2, status3, status4, status5, status_pir, status_dor_win } = req.body;
    if (!device) {
      return res.status(400).json({ error: 'Field "device" wajib diisi' });
    }

    // status..status5 opsional: dipakai ESP melapor kondisi relay setelah jadwal auto / sensor PIR mengubahnya
    // (tidak lewat /api/data lagi, karena /api/data sekarang butuh kode aktivasi milik pelanggan).
    // status_pir / status_dor_win: pembacaan sensor PIR (gerak) & pintu/jendela -- KHUSUS dilaporkan
    // oleh ESP di sini, tidak bisa diubah dari app (bukan relay).
    const onOff = (v) => (v === 'ON' || v === 'OFF' ? v : null);

    // Field yang tidak dikirim ESP (mis. laporan relay saja dari jadwal auto, tanpa suhu/cuaca)
    // dijaga tetap ada nilainya, supaya tidak melanggar NOT NULL saat device BARU pertama kali lapor.
    const before = (await pool.query(
      `SELECT status, status2, status3, status4, suhu, suhu2, suhu3, cuaca FROM iot2 WHERE device = $1`,
      [device]
    )).rows[0] || {};
    const suhuVal  = suhu  !== undefined ? String(suhu)  : (before.suhu  ?? '0');
    const suhu2Val = suhu2 !== undefined ? String(suhu2) : (before.suhu2 ?? '0');
    const suhu3Val = suhu3 !== undefined ? String(suhu3) : (before.suhu3 ?? '0');
    const cuacaVal = cuaca !== undefined ? String(cuaca) : (before.cuaca ?? '-');

    const result = await pool.query(
      `INSERT INTO iot2 (device, status, status2, status3, status4, status5,
                         suhu, suhu2, suhu3, cuaca, status_pir, status_dor_win, last_heartbeat)
       VALUES ($1, COALESCE($2::varchar, 'OFF'), COALESCE($3::varchar, 'OFF'), COALESCE($4::varchar, 'OFF'),
               COALESCE($5::varchar, 'OFF'), COALESCE($6::varchar, 'OFF'),
               $7, $8, $9, $10, COALESCE($11::varchar, 'OFF'), COALESCE($12::varchar, 'OFF'), NULL)
       ON CONFLICT (device) DO UPDATE SET
         status  = COALESCE($2::varchar, iot2.status),
         status2 = COALESCE($3::varchar, iot2.status2),
         status3 = COALESCE($4::varchar, iot2.status3),
         status4 = COALESCE($5::varchar, iot2.status4),
         status5 = COALESCE($6::varchar, iot2.status5),
         suhu = COALESCE(EXCLUDED.suhu, iot2.suhu),
         suhu2 = COALESCE(EXCLUDED.suhu2, iot2.suhu2),
         suhu3 = COALESCE(EXCLUDED.suhu3, iot2.suhu3),
         cuaca = COALESCE(EXCLUDED.cuaca, iot2.cuaca),
         status_pir     = COALESCE($11::varchar, iot2.status_pir),
         status_dor_win = COALESCE($12::varchar, iot2.status_dor_win)
       RETURNING *`,
      [
        device,
        onOff(status), onOff(status2), onOff(status3), onOff(status4), onOff(status5),
        suhuVal, suhu2Val, suhu3Val, cuacaVal,
        onOff(status_pir), onOff(status_dor_win),
      ]
    );

    // Kalau ESP melaporkan perubahan relay (jadwal auto / PIR), perbarui juga pesan MQTT retained.
    // Tanpa ini, snapshot retained di broker BASI (masih status lama dari terakhir kali app menekan
    // sesuatu), dan setiap ESP konek ulang MQTT / restart, status basi itu diterapkan ke relay
    // (mis. lampu yang dinyalakan jadwal auto tiba-tiba mati).
    const relayReported = [status, status2, status3, status4, status5].some((v) => onOff(v) !== null);
    if (relayReported) {
      await publishUpdate(device, mqttPayload(result.rows[0]));
    }

    // Notifikasi (push + Telegram) kalau ada relay (lewat jadwal auto, bukan output PIR) yang berubah.
    // before.status !== undefined memastikan ini bukan heartbeat PERTAMA device (tidak ada "sebelum"-nya).
    if (before.status !== undefined) {
      const savedRow = result.rows[0];
      for (const field of NOTIFY_RELAY_FIELDS) {
        if (before[field] !== savedRow[field]) {
          await notifyRelayChange(device, field, savedRow[field] === 'ON', true);
        }
      }
    }

    // Tidak mengembalikan data device ke pemanggil (endpoint ini tanpa kode aktivasi)
    res.json({ success: true, status_device: withDeviceStatus(result.rows[0]).status_device });
  } catch (err) {
    console.error('Error in POST /api/heartbeat:', err);
    res.status(500).json({ error: 'Gagal menyimpan heartbeat' });
  }
});

// ---------------------------------------------------------------
// POST /api/mqtt-event
// Dipanggil oleh EMQX Rule Engine (action Webhook) HANYA saat ada pesan di
//   smarthome/<device>/online     -> "online" | "offline" (LWT; "offline" dipublish broker)
//   smarthome/<device>/telemetry  -> JSON suhu, cuaca, status_pir, status_dor_win
// Jadi Vercel cuma dipanggil saat status berubah / tiap beberapa menit, bukan tiap 15 detik.
// Keamanan: header X-Webhook-Secret harus sama dengan env MQTT_WEBHOOK_SECRET.
// Body (dari rule SQL: SELECT topic, payload ...): { "topic": "...", "payload": "online" | {...} | "{...}" }
// ---------------------------------------------------------------
function secretMatches(given, expected) {
  const a = Buffer.from(String(given || ''));
  const b = Buffer.from(String(expected || ''));
  return a.length === b.length && crypto.timingSafeEqual(a, b);
}

app.post('/api/mqtt-event', async (req, res) => {
  try {
    const secret = process.env.MQTT_WEBHOOK_SECRET;
    if (!secret) return res.status(503).json({ error: 'MQTT_WEBHOOK_SECRET belum diatur di server' });
    if (!secretMatches(req.get('X-Webhook-Secret'), secret)) return res.sendStatus(403);

    const { topic, payload } = req.body || {};
    // Log ringkas isi event (dipakai untuk diagnosa; volume kecil karena hanya online/offline + telemetry 5 menit)
    console.log('mqtt-event', JSON.stringify({ topic, payload }).slice(0, 400));
    // EMQX Serverless menambahkan awalan "$tenants/<id>/" pada topic pesan Last Will (offline); buang dulu.
    const cleanTopic = String(topic || '').replace(/^\$tenants\/[^/]+\//, '');
    const m = /^smarthome\/([A-Za-z0-9_-]{1,50})\/(online|telemetry)$/.exec(cleanTopic);
    if (!m) { console.error('mqtt-event 400: topic tidak dikenali', JSON.stringify(req.body).slice(0, 400)); return res.status(400).json({ error: 'Topic tidak dikenali' }); }
    const [, device, kind] = m;

    if (kind === 'online') {
      const text = String(payload).trim().toLowerCase();
      if (text !== 'online' && text !== 'offline') {
        console.error('mqtt-event 400: payload bukan online/offline', JSON.stringify({ topic, payload }).slice(0, 400));
        return res.status(400).json({ error: 'Payload harus "online" atau "offline"' });
      }
      const isOnline = text === 'online';
      const prev = (await pool.query(`SELECT is_online FROM iot2 WHERE device = $1`, [device])).rows[0];
      await pool.query(
        `INSERT INTO iot2 (device, is_online, last_heartbeat) VALUES ($1, $2, NOW())
         ON CONFLICT (device) DO UPDATE SET is_online = $2, last_heartbeat = NOW()`,
        [device, isOnline]
      );
      // Notifikasi kalau status benar-benar berubah (best-effort, hanya device milik pemilik bot).
      if (prev && prev.is_online !== isOnline && isOwnTelegramDevice(device)) {
        await sendTelegram(`${isOnline ? '🟢 ONLINE' : '🔴 OFFLINE'} (${device})`);
      }
      return res.json({ success: true, device, status_device: isOnline ? 'online' : 'offline' });
    }

    // telemetry
    let data = payload;
    if (typeof data === 'string') {
      try { data = JSON.parse(data); } catch { return res.status(400).json({ error: 'Payload telemetry bukan JSON' }); }
    }
    if (!data || typeof data !== 'object') return res.status(400).json({ error: 'Payload telemetry tidak valid' });

    const onOff = (v) => (v === 'ON' || v === 'OFF' ? v : null);
    const txt = (v) => (v === undefined || v === null ? null : String(v).slice(0, 20));
    await pool.query(
      `INSERT INTO iot2 (device, suhu, suhu2, suhu3, cuaca, status_pir, status_dor_win, last_heartbeat)
       VALUES ($1, COALESCE($2::varchar, '0'), COALESCE($3::varchar, '0'), COALESCE($4::varchar, '0'),
               COALESCE($5::varchar, '-'), COALESCE($6::varchar, 'OFF'), COALESCE($7::varchar, 'OFF'), NOW())
       ON CONFLICT (device) DO UPDATE SET
         suhu  = COALESCE($2::varchar, iot2.suhu),
         suhu2 = COALESCE($3::varchar, iot2.suhu2),
         suhu3 = COALESCE($4::varchar, iot2.suhu3),
         cuaca = COALESCE($5::varchar, iot2.cuaca),
         status_pir     = COALESCE($6::varchar, iot2.status_pir),
         status_dor_win = COALESCE($7::varchar, iot2.status_dor_win),
         last_heartbeat = NOW()`,
      [device, txt(data.suhu), txt(data.suhu2), txt(data.suhu3), txt(data.cuaca),
       onOff(data.status_pir), onOff(data.status_dor_win)]
    );
    res.json({ success: true, device });
  } catch (err) {
    console.error('Error in POST /api/mqtt-event:', err);
    res.status(500).json({ error: 'Gagal memproses event MQTT' });
  }
});

// ---------------------------------------------------------------
// POST /api/telegram/webhook
// Dipanggil Telegram setiap ada chat masuk ke bot. Kalau pesannya "cek rumah"
// (atau /cek, /status), bot membalas status keseluruhan semua device.
// Keamanan:
//   - header X-Telegram-Bot-Api-Secret-Token harus cocok dengan TELEGRAM_WEBHOOK_SECRET (kalau diisi)
//   - hanya chat dengan id = TELEGRAM_CHAT_ID yang dilayani, chat lain diabaikan diam-diam
// Selalu balas 200 ke Telegram (kalau tidak, Telegram akan mengirim ulang pesan yang sama).
// ---------------------------------------------------------------
app.post('/api/telegram/webhook', async (req, res) => {
  try {
    const secret = process.env.TELEGRAM_WEBHOOK_SECRET;
    if (secret && req.get('X-Telegram-Bot-Api-Secret-Token') !== secret) {
      return res.sendStatus(403);
    }

    const msg = req.body && req.body.message;
    const chatId = msg && msg.chat ? String(msg.chat.id) : null;
    if (!chatId || chatId !== String(process.env.TELEGRAM_CHAT_ID || '')) {
      return res.sendStatus(200); // bukan chat pemilik -> abaikan
    }

    // Normalisasi: huruf kecil, buang "@namabot" (di grup perintah bisa jadi "/cek@namabot"), rapikan spasi
    const text = String(msg.text || '').toLowerCase().replace(/@\w+/g, '').replace(/\s+/g, ' ').trim();
    const isStatusCommand = text.includes('cek rumah') || text === '/cek' || text === '/status' || text === '/cekrumah';

    if (isStatusCommand) {
      // Hanya device milik pemilik bot: daftar ID di TELEGRAM_DEVICE_IDS (pisah koma),
      // mis. "ESP-A1B2C3" atau "ESP-A1B2C3,ESP-D4E5F6". Kosong = semua device (tidak disarankan).
      const ownDevices = ownTelegramDevices();
      const { rows } = ownDevices.length
        ? await pool.query(
            `SELECT DISTINCT ON (device) * FROM iot2 WHERE device = ANY($1::text[]) ORDER BY device, time DESC`,
            [ownDevices]
          )
        : await pool.query(`SELECT DISTINCT ON (device) * FROM iot2 ORDER BY device, time DESC`);
      // WAJIB di-await: di Vercel (serverless), proses bisa dibekukan begitu response dikirim
      await sendTelegram(formatStatusMessage(rows.map(withDeviceStatus)), chatId);
    }

    res.sendStatus(200);
  } catch (err) {
    console.error('Error in POST /api/telegram/webhook:', err);
    res.sendStatus(200);
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
