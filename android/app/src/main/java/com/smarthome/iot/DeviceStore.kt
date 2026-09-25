package com.smarthome.iot

import android.content.Context
import com.google.firebase.messaging.FirebaseMessaging

/**
 * Daftar device milik pelanggan ini (ID + kode aktivasi), disimpan di HP saja.
 * Device yang tidak ada di sini tidak akan pernah tampil di app.
 */
class DeviceStore(context: Context) {

    private val prefs = context.getSharedPreferences("smart_home_devices", Context.MODE_PRIVATE)

    fun list(): List<String> =
        (prefs.getString("list", "") ?: "").split(",").filter { it.isNotBlank() }

    fun codeOf(device: String): String? = prefs.getString("code:$device", null)

    fun add(device: String, code: String) {
        val devices = list().toMutableList()
        if (device !in devices) devices.add(device)
        prefs.edit()
            .putString("list", devices.joinToString(","))
            .putString("code:$device", code)
            .apply()

        // Subscribe topic notifikasi push untuk device ini (lihat PushService & backend/push.js).
        // Best-effort: kalau gagal (mis. tidak ada Google Play Services), fitur lain tetap jalan.
        FirebaseMessaging.getInstance().subscribeToTopic(topicOf(device))
    }

    fun remove(device: String) {
        prefs.edit()
            .putString("list", list().filter { it != device }.joinToString(","))
            .remove("code:$device")
            .apply()
        FirebaseMessaging.getInstance().unsubscribeFromTopic(topicOf(device))
    }

    /** Subscribe ulang semua device tersimpan -- dipanggil sekali saat app dibuka, untuk
     *  menutup celah kalau subscribe sebelumnya gagal (mis. saat itu tidak ada internet). */
    fun resubscribeAll() {
        for (device in list()) FirebaseMessaging.getInstance().subscribeToTopic(topicOf(device))
    }

    companion object {
        // HARUS SAMA PERSIS dengan topicOf() di backend/push.js
        fun topicOf(device: String): String =
            "device_" + device.replace(Regex("""[^a-zA-Z0-9\-_.~%]"""), "_")
    }
}
