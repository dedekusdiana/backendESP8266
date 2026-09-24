package com.smarthome.iot

import android.app.TimePickerDialog
import android.content.res.ColorStateList
import android.graphics.Color
import android.view.LayoutInflater
import android.view.ViewGroup
import androidx.recyclerview.widget.RecyclerView
import com.smarthome.iot.databinding.ItemAutoBinding

/** Daftar kartu jadwal otomatis (Auto 1, Auto 2) yang tampil di atas Kontrol Relay. */
class AutoAdapter(
    private val onChange: (jsonKey: String, newRaw: String) -> Unit
) : RecyclerView.Adapter<AutoAdapter.AutoViewHolder>() {

    private var currentItem: DeviceStatusItem? = null
    private var deviceOnline: Boolean = true

    fun submitStatus(item: DeviceStatusItem, isDeviceOnline: Boolean) {
        currentItem = item
        deviceOnline = isDeviceOnline
        notifyItemRangeChanged(0, AUTO_FIELDS.size)
    }

    override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): AutoViewHolder {
        val binding = ItemAutoBinding.inflate(LayoutInflater.from(parent.context), parent, false)
        return AutoViewHolder(binding, onChange)
    }

    override fun onBindViewHolder(holder: AutoViewHolder, position: Int) {
        val field = AUTO_FIELDS[position]
        val schedule = AutoSchedule.parse(currentItem?.valueForAuto(field.jsonKey))
        // Status lampu = status relay yang dikendalikan auto ini (Auto 1 -> Relay 1, Auto 2 -> Relay 2)
        val lampOn = currentItem?.valueFor(field.relayKey).equals("ON", ignoreCase = true)
        holder.bind(field, schedule, lampOn, deviceOnline)
    }

    override fun getItemCount(): Int = AUTO_FIELDS.size

    class AutoViewHolder(
        private val binding: ItemAutoBinding,
        private val onChange: (jsonKey: String, newRaw: String) -> Unit
    ) : RecyclerView.ViewHolder(binding.root) {

        fun bind(field: AutoField, schedule: AutoSchedule, lampOn: Boolean, deviceOnline: Boolean) {
            binding.tvAutoTitle.text = field.label

            // Indikator lampu
            binding.tvAutoLamp.text = if (lampOn) "\u25CF Menyala" else "\u25CF Mati"
            binding.tvAutoLamp.setTextColor(Color.parseColor(if (lampOn) "#2E7D32" else "#90A4AE"))
            binding.tvAutoIcon.backgroundTintList =
                ColorStateList.valueOf(Color.parseColor(if (lampOn) "#FFE082" else "#ECEFF1"))
            binding.tvAutoIcon.alpha = if (lampOn) 1.0f else 0.6f

            binding.tvAutoSummary.text = if (schedule.enabled) "\u2022 Jadwal aktif" else "\u2022 Jadwal nonaktif"

            binding.btnAutoOn.text = "Nyala  ${schedule.onTime}"
            binding.btnAutoOff.text = "Mati  ${schedule.offTime}"

            // Lepas listener dulu supaya setChecked() dari data server tidak memicu kirim ulang
            binding.switchAuto.setOnCheckedChangeListener(null)
            binding.switchAuto.isChecked = schedule.enabled
            binding.switchAuto.setOnCheckedChangeListener { _, checked ->
                onChange(field.jsonKey, schedule.copy(enabled = checked).toRaw())
            }

            // Device offline -> kontrol auto dinonaktifkan & diredupkan, sama seperti kontrol relay
            binding.switchAuto.isEnabled = deviceOnline
            binding.btnAutoOn.isEnabled = deviceOnline
            binding.btnAutoOff.isEnabled = deviceOnline
            binding.root.alpha = if (deviceOnline) 1.0f else 0.5f

            binding.btnAutoOn.setOnClickListener {
                pickTime(schedule.onTime) { picked ->
                    onChange(field.jsonKey, schedule.copy(onTime = picked).toRaw())
                }
            }
            binding.btnAutoOff.setOnClickListener {
                pickTime(schedule.offTime) { picked ->
                    onChange(field.jsonKey, schedule.copy(offTime = picked).toRaw())
                }
            }
        }

        private fun pickTime(current: String, onPicked: (String) -> Unit) {
            val parts = current.split(":")
            val hour = parts.getOrNull(0)?.toIntOrNull() ?: 18
            val minute = parts.getOrNull(1)?.toIntOrNull() ?: 0
            TimePickerDialog(
                binding.root.context,
                { _, h, m -> onPicked(String.format(java.util.Locale.US, "%02d:%02d", h, m)) },
                hour, minute, true // format 24 jam
            ).show()
        }
    }
}
