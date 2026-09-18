package com.smarthome.iot

import com.google.gson.annotations.SerializedName

/** Satu device di GET /api/status (daftar semua device). */
data class DeviceStatusItem(
    @SerializedName("device") val device: String,
    @SerializedName("status") val status: String,
    @SerializedName("time") val time: String
)

data class StatusListResponse(
    @SerializedName("devices") val devices: List<DeviceStatusItem>
)

/** Body untuk POST /api/data */
data class UpdateStatusRequest(
    @SerializedName("device") val device: String,
    @SerializedName("status") val status: String
)

data class UpdateStatusResponse(
    @SerializedName("success") val success: Boolean,
    @SerializedName("data") val data: DeviceStatusItem?
)
