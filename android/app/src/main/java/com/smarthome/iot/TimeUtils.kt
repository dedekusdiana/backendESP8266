package com.smarthome.iot

import java.text.SimpleDateFormat
import java.util.Locale
import java.util.TimeZone

object TimeUtils {

    private val isoParsers = listOf(
        "yyyy-MM-dd'T'HH:mm:ss.SSS'Z'",
        "yyyy-MM-dd'T'HH:mm:ss'Z'"
    )

    private val outputFormat = SimpleDateFormat("dd MMM yyyy, HH:mm:ss", Locale("id", "ID")).apply {
        timeZone = TimeZone.getTimeZone("Asia/Jakarta")
    }

    /** Ubah timestamp UTC dari server (format ISO8601) jadi jam Jakarta (WIB / UTC+7). */
    fun toJakartaTime(isoUtc: String?): String {
        if (isoUtc.isNullOrBlank() || isoUtc == "-") return "-"

        for (pattern in isoParsers) {
            try {
                val parser = SimpleDateFormat(pattern, Locale.US).apply {
                    timeZone = TimeZone.getTimeZone("UTC")
                }
                val date = parser.parse(isoUtc)
                if (date != null) {
                    return outputFormat.format(date) + " WIB"
                }
            } catch (e: Exception) {
                // coba pattern berikutnya
            }
        }
        return isoUtc // fallback kalau format tidak dikenali
    }
}
