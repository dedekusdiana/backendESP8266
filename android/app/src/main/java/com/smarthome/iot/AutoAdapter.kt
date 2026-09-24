package com.smarthome.iot

import android.app.TimePickerDialog
import android.view.LayoutInflater
import android.view.ViewGroup
import androidx.recyclerview.widget.RecyclerView
import com.smarthome.iot.databinding.ItemAutoBinding

/** Daftar kartu jadwal otomatis (Auto 1, Auto 2) yang tampil di atas Kontrol Relay. */
class AutoAdapter(
    private val labelPrefs: LabelPrefs,
    private val onChange: (jsonKey: String, newRaw: String) -> Unit
) : RecyclerView.Adapter<AutoAdapter.AutoViewHolder>() {

    private var currentDevice: String = ""
    private var currentItem: DeviceStatusItem? = null

    fun submitStatus(device: String, item: DeviceStatusItem) {
        currentDevice = device
        currentItem = item
        notifyItemRangeChanged(0, AUTO_FIELDS.size)
    }

    /** Dipanggil setelah user rename relay, supaya judul kartu auto ikut berubah. */
    fun refreshLabels() {
        notifyItemRangeChanged(0, AUTO_FIELDS.size)
    }

    override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): AutoViewHolder {
        val binding = ItemAutoBinding.inflate(LayoutInflater.from(parent.context), parent, false)
        return AutoViewHolder(binding, onChange)
    }

    override fun onBindViewHolder(holder: AutoViewHolder, position: Int) {
        val field = AUTO_FIELDS[position]
        val schedule = AutoSchedule.parse(currentItem?.valueForAuto(field.jsonKey))
        val relayDefault = RELAY_FIELDS.firstOrNull { it.jsonKey == field.relayKey }?.label ?: field.relayKey
        val relayLabel = labelPrefs.getLabel(currentDevice, field.relayKey, relayDefault)
        holder.bind(field, relayLabel, schedule)
    }

    override fun getItemCount(): Int = AUTO_FIELDS.size

    class AutoViewHolder(
        private val binding: ItemAutoBinding,
        private val onChange: (jsonKey: String, newRaw: String) -> Unit
    ) : RecyclerView.ViewHolder(binding.root) {

        fun bind(field: AutoField, relayLabel: String, schedule: AutoSchedule) {
            binding.tvAutoTitle.text = relayLabel
            binding.tvAutoSummary.text = "${field.label} \u2022 " +
                if (schedule.enabled) "Aktif, setiap hari" else "Nonaktif"

            binding.btnAutoOn.text = "Nyala  ${schedule.onTime}"
            binding.btnAutoOff.text = "Mati  ${schedule.offTime}"

            // Lepas listener dulu supaya setChecked() dari data server tidak memicu kirim ulang
            binding.switchAuto.setOnCheckedChangeListener(null)
            binding.switchAuto.isChecked = schedule.enabled
            binding.switchAuto.setOnCheckedChangeListener { _, checked ->
                onChange(field.jsonKey, schedule.copy(enabled = checked).toRaw())
            }

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
