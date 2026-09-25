package com.smarthome.iot

import android.content.Context

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
    }

    fun remove(device: String) {
        prefs.edit()
            .putString("list", list().filter { it != device }.joinToString(","))
            .remove("code:$device")
            .apply()
    }
}
