package com.smarthome.iot

import android.content.res.ColorStateList
import android.graphics.Color
import android.view.LayoutInflater
import android.view.ViewGroup
import androidx.recyclerview.widget.RecyclerView
import com.smarthome.iot.databinding.ItemSensorBinding

/**
 * Daftar kartu sensor read-only (PIR & Pintu/Jendela). BEDA dari RelayAdapter: tidak ada
 * switch/tombol sama sekali, karena ini cuma pembacaan dari ESP, bukan sesuatu yang bisa
 * dinyalakan/dimatikan dari app.
 */
class SensorAdapter : RecyclerView.Adapter<SensorAdapter.SensorViewHolder>() {

    private var currentItem: DeviceStatusItem? = null

    fun submitStatus(item: DeviceStatusItem) {
        currentItem = item
        notifyItemRangeChanged(0, SENSOR_FIELDS.size)
    }

    override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): SensorViewHolder {
        val binding = ItemSensorBinding.inflate(LayoutInflater.from(parent.context), parent, false)
        return SensorViewHolder(binding)
    }

    override fun onBindViewHolder(holder: SensorViewHolder, position: Int) {
        val field = SENSOR_FIELDS[position]
        val active = currentItem?.valueForSensor(field.jsonKey).equals("ON", ignoreCase = true)
        holder.bind(field, active)
    }

    override fun getItemCount(): Int = SENSOR_FIELDS.size

    class SensorViewHolder(private val binding: ItemSensorBinding) : RecyclerView.ViewHolder(binding.root) {
        fun bind(field: SensorField, active: Boolean) {
            binding.tvSensorIcon.text = field.icon
            binding.tvSensorLabel.text = field.label

            val text = if (active) field.onText else field.offText
            binding.tvSensorStatus.text = text

            // Merah kalau kondisi "perlu perhatian" (ada gerak / pintu-jendela terbuka), hijau kalau aman
            val bgColor = if (active) "#FFEBEE" else "#E8F5E9"
            val textColor = if (active) "#C62828" else "#2E7D32"
            binding.tvSensorStatus.backgroundTintList = ColorStateList.valueOf(Color.parseColor(bgColor))
            binding.tvSensorStatus.setTextColor(Color.parseColor(textColor))
        }
    }
}
