package com.smarthome.iot

import retrofit2.Response
import retrofit2.http.Body
import retrofit2.http.GET
import retrofit2.http.Header
import retrofit2.http.POST
import retrofit2.http.Path

interface ApiService {

    // Semua akses ke data device wajib membawa kode aktivasi device itu (header X-Device-Code).
    // Daftar semua device TIDAK tersedia untuk app -- app hanya tahu device yang sudah
    // "ditambahkan" pelanggan lewat kode di stiker (disimpan lokal di HP, lihat DeviceStore).
    @GET("api/status/{device}")
    suspend fun getStatus(
        @Path("device") device: String,
        @Header("X-Device-Code") code: String
    ): Response<DeviceStatusItem>

    // Body fleksibel: cukup kirim "device" + field yang mau diubah saja,
    // misal {"device":"ESP-A1B2C3","status":"ON"} -- field lain tidak perlu disertakan.
    @POST("api/data")
    suspend fun updateStatus(
        @Header("X-Device-Code") code: String,
        @Body body: Map<String, String>
    ): Response<UpdateStatusResponse>

    // Verifikasi ID device + kode aktivasi saat pelanggan menambahkan device.
    @POST("api/claim")
    suspend fun claim(@Body body: Map<String, String>): Response<ClaimResponse>
}
