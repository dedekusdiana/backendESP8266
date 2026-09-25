const admin = require('firebase-admin');

// Notifikasi dikirim ke TOPIC per device (bukan menyimpan token per HP), supaya:
// - tidak perlu tabel/kolom baru untuk token,
// - beberapa HP yang menambahkan device yang sama otomatis dapat notifikasi yang sama.
// App Android subscribe ke topic ini saat pelanggan menambahkan device (lihat DeviceStore.add()).
function topicOf(device) {
  // Nama topic FCM hanya boleh [a-zA-Z0-9-_.~%]
  return 'device_' + String(device).replace(/[^a-zA-Z0-9\-_.~%]/g, '_');
}

let ready = false;
function init() {
  if (ready) return true;
  const raw = process.env.FIREBASE_SERVICE_ACCOUNT_JSON;
  if (!raw) return false; // belum dikonfigurasi -> notifikasi dilewati diam-diam
  try {
    const serviceAccount = JSON.parse(raw);
    admin.initializeApp({ credential: admin.credential.cert(serviceAccount) });
    ready = true;
    return true;
  } catch (err) {
    console.error('FIREBASE_SERVICE_ACCOUNT_JSON tidak valid:', err.message);
    return false;
  }
}

// Kirim notifikasi ke semua HP yang menambahkan `device`. Best-effort: gagal kirim
// tidak boleh menggagalkan request utama (update relay / heartbeat tetap harus sukses).
async function notifyDevice(device, title, body) {
  if (!init()) return;
  try {
    await admin.messaging().send({
      topic: topicOf(device),
      notification: { title, body },
      android: { priority: 'high' },
    });
  } catch (err) {
    console.error('Gagal kirim notifikasi:', err.message);
  }
}

module.exports = { notifyDevice, topicOf };
