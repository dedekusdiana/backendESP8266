// Notifikasi lewat Telegram Bot -- jauh lebih simpel dari Firebase: tidak perlu ubah APK,
// tidak perlu service account, cukup satu request HTTP ke api.telegram.org.
//
// Setup (sekali saja, lihat README bagian "Notifikasi Telegram"):
//   1. Chat @BotFather di Telegram -> /newbot -> dapat TELEGRAM_BOT_TOKEN
//   2. Chat bot barumu sekali (apa saja) -> buka
//      https://api.telegram.org/bot<TOKEN>/getUpdates -> cari "chat":{"id": ...} -> TELEGRAM_CHAT_ID
//   3. Isi keduanya di Environment Variables Vercel, lalu redeploy.

async function sendTelegram(text, chatIdOverride = null) {
  const token = process.env.TELEGRAM_BOT_TOKEN;
  const chatId = chatIdOverride || process.env.TELEGRAM_CHAT_ID;
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


async function answerTelegramChat(chatId, text) {
  return sendTelegram(text, chatId);
}

module.exports = { sendTelegram, answerTelegramChat };
