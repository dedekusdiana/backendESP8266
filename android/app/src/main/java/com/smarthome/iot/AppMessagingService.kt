package com.smarthome.iot

import android.Manifest
import android.content.pm.PackageManager
import androidx.core.app.NotificationCompat
import androidx.core.app.NotificationManagerCompat
import androidx.core.content.ContextCompat
import com.google.firebase.messaging.FirebaseMessagingService
import com.google.firebase.messaging.RemoteMessage

/**
 * Saat app TERBUKA (foreground), FCM tidak menampilkan notifikasi otomatis -- pesan hanya diserahkan ke
 * onMessageReceived(). Tanpa service ini, notifikasi cuma muncul kalau app ditutup. Di sini kita
 * tampilkan sendiri supaya perilakunya sama, baik app terbuka maupun tertutup.
 * (Saat app tertutup, sistem Android yang menampilkan notifikasinya; fungsi ini tidak dipanggil.)
 */
class AppMessagingService : FirebaseMessagingService() {

    override fun onMessageReceived(message: RemoteMessage) {
        val title = message.notification?.title ?: message.data["title"] ?: return
        val body = message.notification?.body ?: message.data["body"] ?: ""

        if (ContextCompat.checkSelfPermission(this, Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED &&
            android.os.Build.VERSION.SDK_INT >= 33
        ) return

        // Pastikan channel ada (idempotent), lalu tampilkan
        Push.setup(this, emptyList())

        val notification = NotificationCompat.Builder(this, Push.CHANNEL_ID)
            .setSmallIcon(R.drawable.ic_relay)
            .setContentTitle(title)
            .setContentText(body)
            .setPriority(NotificationCompat.PRIORITY_HIGH)
            .setAutoCancel(true)
            .build()

        NotificationManagerCompat.from(this).notify(System.currentTimeMillis().toInt(), notification)
    }
}
