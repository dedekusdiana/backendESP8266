package com.smarthome.iot

import com.google.gson.annotations.SerializedName

/** Satu baris data device (dari GET /api/status, /api/status/:device, dll). */
data class DeviceStatusItem(
    @SerializedName("device") val device: String,
    @SerializedName("status") val status: String,
    @SerializedName("status2") val status2: String,
    @SerializedName("status3") val status3: String,
    @SerializedName("status4") val status4: String,
    @SerializedName("status5") val status5: String,
    @SerializedName("suhu") val suhu: String,
    @SerializedName("suhu2") val suhu2: String,
    @SerializedName("suhu3") val suhu3: String,
    @SerializedName("cuaca") val cuaca: String? = null,
    @SerializedName("time") val time: String,
    @SerializedName("status_device") val statusDevice: String? = null
)

data class StatusListResponse(
    @SerializedName("devices") val devices: List<DeviceStatusItem>
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
    RelayField("Relay 5", "status5"),
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
    "status5" -> status5
    else -> "OFF"
}
