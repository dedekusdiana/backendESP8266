// Notifikasi lewat Telegram Bot -- jauh lebih simpel dari Firebase: tidak perlu ubah APK,
// tidak perlu service account, cukup satu request HTTP ke api.telegram.org.
//
// Setup (sekali saja, lihat README bagian "Notifikasi Telegram"):
//   1. Chat @BotFather di Telegram -> /newbot -> dapat TELEGRAM_BOT_TOKEN
//   2. Chat bot barumu sekali (apa saja) -> buka
//      https://api.telegram.org/bot<TOKEN>/getUpdates -> cari "chat":{"id": ...} -> TELEGRAM_CHAT_ID
//   3. Isi keduanya di Environment Variables Vercel, lalu redeploy.

async function sendTelegram(text, chatId = process.env.TELEGRAM_CHAT_ID) {
  const token = process.env.TELEGRAM_BOT_TOKEN;
  if (!token || !chatId) return; // belum dikonfigurasi -> dilewati diam-diam, fitur lain tetap jalan

  try {
    const res = await fetch(`https://api.telegram.org/bot${token}/sendMessage`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ chat_id: chatId, text }),
    });
    if (!res.ok) {
      const body = await res.text();
      console.error('Gagal kirim Telegram:', res.status, body);
    }
  } catch (err) {
    console.error('Gagal kirim Telegram:', err.message);
  }
}

// ---------------------------------------------------------------
// Format laporan status keseluruhan untuk perintah "cek rumah".
// `rows` = baris terakhir tiap device dari tabel iot2 (sudah lewat withDeviceStatus,
// jadi punya field status_device: 'online' | 'offline').
// Teks polos (tanpa parse_mode) supaya karakter seperti "_" atau "." tidak bikin error format.
// ---------------------------------------------------------------
const RELAY_NAMES = [
  ['status', 'Relay 1'],
  ['status2', 'Relay 2'],
  ['status3', 'Relay 3'],
  ['status4', 'Relay 4'],
  ['status5', 'Relay 5 (output PIR)'],
];

function formatAgo(date) {
  if (!date) return 'belum pernah';
  const sec = Math.max(0, Math.round((Date.now() - new Date(date).getTime()) / 1000));
  if (sec < 60) return `${sec} detik lalu`;
  if (sec < 3600) return `${Math.floor(sec / 60)} menit lalu`;
  if (sec < 86400) return `${Math.floor(sec / 3600)} jam lalu`;
  return `${Math.floor(sec / 86400)} hari lalu`;
}

function formatAuto(value) {
  // Format: "<ON|OFF>,<HH:MM nyala>,<HH:MM mati>"
  const [mode, on, off] = String(value || '').split(',');
  if (!on || !off) return String(value || '-');
  return `${mode === 'ON' ? 'AKTIF' : 'nonaktif'} (nyala ${on}, mati ${off})`;
}

function formatDevice(row) {
  const online = row.status_device === 'online';
  const lines = [];
  lines.push(`🏠 ${row.device}`);
  lines.push(`${online ? '🟢 ONLINE' : '🔴 OFFLINE'} - kontak terakhir ${formatAgo(row.last_heartbeat)}`);
  if (!online) lines.push('⚠️ Data di bawah adalah kondisi terakhir sebelum device terputus.');

  lines.push('', '💡 Relay');
  for (const [field, label] of RELAY_NAMES) {
    const on = row[field] === 'ON';
    lines.push(`${on ? '🟢' : '⚪'} ${label}: ${on ? 'ON' : 'OFF'}`);
  }

  lines.push('', '⏰ Jadwal Auto');
  lines.push(`Auto 1 (Relay 1): ${formatAuto(row.auto1)}`);
  lines.push(`Auto 2 (Relay 2): ${formatAuto(row.auto2)}`);

  lines.push('', '📟 Sensor');
  lines.push(`🌡️ Suhu: ${row.suhu}°C | ${row.suhu2}°C | ${row.suhu3}°C`);
  lines.push(`🌦️ Cuaca: ${row.cuaca || '-'}`);
  lines.push(`🚶 Gerak (PIR): ${row.status_pir === 'ON' ? 'ADA GERAK ⚠️' : 'Aman'}`);
  lines.push(`🚪 Pintu/Jendela: ${row.status_dor_win === 'ON' ? 'BUKA ⚠️' : 'Tutup'}`);
  return lines.join('\n');
}

function formatStatusMessage(rows) {
  if (!rows || rows.length === 0) return 'Belum ada data device sama sekali.';
  return rows.map(formatDevice).join('\n\n--------------------\n\n');
}

module.exports = { sendTelegram, formatStatusMessage };
