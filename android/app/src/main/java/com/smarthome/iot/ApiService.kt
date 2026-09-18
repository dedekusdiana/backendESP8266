package com.smarthome.iot

import retrofit2.Response
import retrofit2.http.Body
import retrofit2.http.GET
import retrofit2.http.POST
import retrofit2.http.Path

interface ApiService {
    @GET("api/status")
    suspend fun getAllStatus(): Response<StatusListResponse>

    @GET("api/status/{device}")
    suspend fun getStatus(@Path("device") device: String): Response<DeviceStatusItem>

    @POST("api/data")
    suspend fun updateStatus(@Body request: UpdateStatusRequest): Response<UpdateStatusResponse>
}
