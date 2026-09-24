// Membuat kode aktivasi untuk stiker produk.
// Pakai:  node scripts/generate-code.js ESP-A1B2C3 ESP-D4E5F6 ...
// (nama device = yang muncul di Serial Monitor: "Device: ESP-XXXXXX")
// DEVICE_CODE_SECRET dibaca dari backend/.env -- HARUS sama dengan yang di Vercel.
require('dotenv').config({ path: require('path').join(__dirname, '..', '.env') });
const { deviceCode } = require('../auth');

const devices = process.argv.slice(2);
if (devices.length === 0) {
  console.log('Pakai: node scripts/generate-code.js ESP-A1B2C3 [ESP-D4E5F6 ...]');
  process.exit(1);
}
for (const d of devices) {
  console.log(`ID: ${d}    Kode: ${deviceCode(d)}`);
}
