package com.smarthome.iot

import android.content.Context

/** Nyimpen label custom yang diedit user (mis. "Relay 1" -> "Lampu Teras") per device, di HP saja. */
class LabelPrefs(context: Context) {

    private val prefs = context.getSharedPreferences("smart_home_labels", Context.MODE_PRIVATE)

    private fun key(device: String, field: String) = "label:$device:$field"

    fun getLabel(device: String, field: String, defaultLabel: String): String {
        return prefs.getString(key(device, field), defaultLabel) ?: defaultLabel
    }

    fun setLabel(device: String, field: String, label: String) {
        prefs.edit().putString(key(device, field), label).apply()
    }
}
