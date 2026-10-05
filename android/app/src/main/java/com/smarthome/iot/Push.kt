package com.smarthome.iot

import android.Manifest
import android.app.NotificationChannel
import android.app.NotificationManager
import android.content.Context
import android.content.pm.PackageManager
import android.os.Build
import androidx.core.content.ContextCompat
import com.google.firebase.messaging.FirebaseMessaging

/**
 * Push notifikasi (FCM) per device lewat TOPIC. Nama topic HARUS sama dengan topicOf() di backend/push.js,
 * jadi semua HP yang menambahkan device yang sama otomatis menerima notifikasi yang sama.
 */
object Push {
    const val CHANNEL_ID = "relay_alerts"

    fun topicOf(device: String): String =
        "device_" + device.replace(Regex("[^a-zA-Z0-9\\-_.~%]"), "_")

    fun subscribe(device: String) {
        FirebaseMessaging.getInstance().subscribeToTopic(topicOf(device))
    }

    fun unsubscribe(device: String) {
        FirebaseMessaging.getInstance().unsubscribeFromTopic(topicOf(device))
    }

    /** Dipanggil tiap app dibuka: pastikan channel ada dan semua device yang tersimpan ter-subscribe (idempotent). */
    fun setup(context: Context, devices: List<String>) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val channel = NotificationChannel(CHANNEL_ID, "Notifikasi relay", NotificationManager.IMPORTANCE_HIGH)
            context.getSystemService(NotificationManager::class.java).createNotificationChannel(channel)
        }
        devices.forEach { subscribe(it) }
    }

    /** Android 13+ butuh izin runtime POST_NOTIFICATIONS. */
    fun needsPermission(context: Context): Boolean =
        Build.VERSION.SDK_INT >= 33 &&
            ContextCompat.checkSelfPermission(context, Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED
}
