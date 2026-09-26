package com.smarthome.iot

import com.google.gson.annotations.SerializedName

/** Satu baris data device (dari GET /api/status, /api/status/:device, dll). */
data class DeviceStatusItem(
    @SerializedName("device") val device: String,
    @SerializedName("status") val status: String,
    @SerializedName("status2") val status2: String,
    @SerializedName("status3") val status3: String,
    @SerializedName("status4") val status4: String,
    @SerializedName("suhu") val suhu: String,
    @SerializedName("suhu2") val suhu2: String,
    @SerializedName("suhu3") val suhu3: String,
    @SerializedName("cuaca") val cuaca: String? = null,
    @SerializedName("auto1") val auto1: String? = null,
    @SerializedName("auto2") val auto2: String? = null,
    @SerializedName("time") val time: String,
    @SerializedName("status_device") val statusDevice: String? = null,
    // Sensor (bukan relay -- read-only, dilaporkan ESP lewat heartbeat)
    @SerializedName("status_pir") val statusPir: String? = null,
    @SerializedName("status_dor_win") val statusDorWin: String? = null
)

data class StatusListResponse(
    @SerializedName("devices") val devices: List<DeviceStatusItem>
)

/** Hasil POST /api/claim. data = null kalau device belum pernah online (kode tetap valid). */
data class ClaimResponse(
    @SerializedName("success") val success: Boolean,
    @SerializedName("data") val data: DeviceStatusItem?
)

data class UpdateStatusResponse(
    @SerializedName("success") val success: Boolean,
    @SerializedName("data") val data: DeviceStatusItem?
)

/** Nama tampilan + key JSON untuk tiap relay, dipakai untuk isi daftar 5 relay di UI. */
data class RelayField(val label: String, val jsonKey: String)

val RELAY_FIELDS = listOf(
    RelayField("Relay 1", "status"),
    RelayField("Relay 2", "status2"),
    RelayField("Relay 3", "status3"),
    RelayField("Relay 4", "status4"),
)

data class SuhuField(val label: String, val jsonKey: String)

val SUHU_FIELDS = listOf(
    SuhuField("Suhu 1", "suhu"),
    SuhuField("Suhu 2", "suhu2"),
    SuhuField("Suhu 3", "suhu3"),
)

fun DeviceStatusItem.valueForSuhu(jsonKey: String): String = when (jsonKey) {
    "suhu" -> suhu
    "suhu2" -> suhu2
    "suhu3" -> suhu3
    else -> "0"
}

fun DeviceStatusItem.valueFor(jsonKey: String): String = when (jsonKey) {
    "status" -> status
    "status2" -> status2
    "status3" -> status3
    "status4" -> status4
    else -> "OFF"
}

/**
 * Jadwal otomatis. Isi field di database: "<ON|OFF>,<jam nyala>,<jam mati>", mis. "ON,18:00,06:00".
 * relayKey = relay yang dikendalikan (dipakai juga untuk mengambil nama relay yang sudah diedit user).
 */
data class AutoField(val label: String, val jsonKey: String, val relayKey: String)

val AUTO_FIELDS = listOf(
    AutoField("Auto 1", "auto1", "status"),   // -> Relay 1
    AutoField("Auto 2", "auto2", "status2"),  // -> Relay 2
)

const val AUTO_DEFAULT = "OFF,18:00,06:00"

data class AutoSchedule(val enabled: Boolean, val onTime: String, val offTime: String) {
    fun toRaw(): String = "${if (enabled) "ON" else "OFF"},$onTime,$offTime"

    companion object {
        fun parse(raw: String?): AutoSchedule {
            val parts = (raw ?: AUTO_DEFAULT).split(",").map { it.trim() }
            if (parts.size != 3) return parse(AUTO_DEFAULT)
            return AutoSchedule(parts[0].equals("ON", ignoreCase = true), parts[1], parts[2])
        }
    }
}

fun DeviceStatusItem.valueForAuto(jsonKey: String): String = when (jsonKey) {
    "auto1" -> auto1 ?: AUTO_DEFAULT
    "auto2" -> auto2 ?: AUTO_DEFAULT
    else -> AUTO_DEFAULT
}

/**
 * Sensor read-only (BUKAN relay -- tidak ada tombol nyala/mati dari app, cuma pembacaan dari ESP).
 * onText/offText = teks yang ditampilkan, beda-beda tiap sensor (bukan cuma "ON"/"OFF").
 */
data class SensorField(val label: String, val jsonKey: String, val onText: String, val offText: String, val icon: String)

val SENSOR_FIELDS = listOf(
    SensorField("Sensor Gerak (PIR)", "status_pir", "Ada Gerak", "Aman", "\uD83D\uDEB6"),
    SensorField("Pintu / Jendela", "status_dor_win", "Buka", "Tutup", "\uD83D\uDEAA"),
)

fun DeviceStatusItem.valueForSensor(jsonKey: String): String = when (jsonKey) {
    "status_pir" -> statusPir ?: "OFF"
    "status_dor_win" -> statusDorWin ?: "OFF"
    else -> "OFF"
}
