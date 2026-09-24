const crypto = require('crypto');

// Kode aktivasi per device, diturunkan dari nama device + DEVICE_CODE_SECRET (HMAC-SHA256).
// Tidak disimpan di database -- dihitung ulang tiap kali. Kode dicetak di stiker produk
// (pakai scripts/generate-code.js) dan dimasukkan pelanggan di app untuk "menambahkan" device.
// Alfabet tanpa 0/O/1/I supaya tidak salah baca. 10 karakter = 50 bit (tidak bisa ditebak).
const CODE_ALPHABET = 'ABCDEFGHJKLMNPQRSTUVWXYZ23456789';

function deviceCode(device) {
  const secret = process.env.DEVICE_CODE_SECRET;
  if (!secret) throw new Error('DEVICE_CODE_SECRET belum diisi di environment');
  const digest = crypto.createHmac('sha256', secret).update(String(device)).digest();
  let out = '';
  for (let i = 0; i < 10; i++) out += CODE_ALPHABET[digest[i] % CODE_ALPHABET.length];
  return `${out.slice(0, 5)}-${out.slice(5)}`;
}

function normalize(code) {
  return String(code || '').toUpperCase().replace(/[^A-Z0-9]/g, '');
}

function safeEqual(a, b) {
  const bufA = Buffer.from(a);
  const bufB = Buffer.from(b);
  return bufA.length === bufB.length && crypto.timingSafeEqual(bufA, bufB);
}

function codeMatches(device, provided) {
  return safeEqual(normalize(provided), normalize(deviceCode(device)));
}

// Middleware: request harus membawa header X-Device-Code yang cocok dengan device-nya
// (device diambil dari :device di URL, atau field "device" di body).
function requireDeviceCode(req, res, next) {
  const device = req.params.device || (req.body && req.body.device);
  const provided = req.get('X-Device-Code');
  if (!device || !provided) {
    return res.status(401).json({ error: 'Device dan kode aktivasi wajib diisi' });
  }
  try {
    if (!codeMatches(device, provided)) {
      return res.status(403).json({ error: 'Kode aktivasi salah' });
    }
  } catch (err) {
    console.error('requireDeviceCode:', err.message);
    return res.status(500).json({ error: 'Server belum dikonfigurasi' });
  }
  next();
}

// Middleware khusus admin (daftar semua device). Kalau ADMIN_API_KEY tidak diisi, endpoint tertutup.
function requireAdmin(req, res, next) {
  const key = process.env.ADMIN_API_KEY;
  const provided = req.get('X-Admin-Key');
  if (!key || !provided || !safeEqual(provided, key)) {
    return res.status(403).json({ error: 'Akses ditolak' });
  }
  next();
}

module.exports = { deviceCode, codeMatches, requireDeviceCode, requireAdmin };
