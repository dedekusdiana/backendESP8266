package com.smarthome.iot

import android.graphics.Color
import android.view.LayoutInflater
import android.view.ViewGroup
import androidx.recyclerview.widget.RecyclerView
import com.smarthome.iot.databinding.ItemRelayBinding

class RelayAdapter(
    private val labelPrefs: LabelPrefs,
    private val onToggle: (jsonKey: String, newStatus: String) -> Unit,
    private val onEditLabel: (jsonKey: String, currentLabel: String) -> Unit
) : RecyclerView.Adapter<RelayAdapter.RelayViewHolder>() {

    private var currentDevice: String = ""
    private var currentItem: DeviceStatusItem? = null
    private var deviceOnline: Boolean = true

    fun submitStatus(device: String, item: DeviceStatusItem, isDeviceOnline: Boolean) {
        currentDevice = device
        currentItem = item
        deviceOnline = isDeviceOnline
        notifyItemRangeChanged(0, RELAY_FIELDS.size)
    }

    /** Dipanggil setelah user rename label, supaya baris itu langsung refresh. */
    fun refreshLabels() {
        notifyItemRangeChanged(0, RELAY_FIELDS.size)
    }

    override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): RelayViewHolder {
        val binding = ItemRelayBinding.inflate(LayoutInflater.from(parent.context), parent, false)
        return RelayViewHolder(binding, onToggle, onEditLabel)
    }

    override fun onBindViewHolder(holder: RelayViewHolder, position: Int) {
        val field = RELAY_FIELDS[position]
        val value = currentItem?.valueFor(field.jsonKey) ?: "OFF"
        val label = labelPrefs.getLabel(currentDevice, field.jsonKey, field.label)
        holder.bind(field, label, value, deviceOnline)
    }

    override fun getItemCount(): Int = RELAY_FIELDS.size

    class RelayViewHolder(
        private val binding: ItemRelayBinding,
        private val onToggle: (jsonKey: String, newStatus: String) -> Unit,
        private val onEditLabel: (jsonKey: String, currentLabel: String) -> Unit
    ) : RecyclerView.ViewHolder(binding.root) {

        fun bind(field: RelayField, label: String, value: String, deviceOnline: Boolean) {
            binding.tvRelayLabel.text = label

            val isOn = value.equals("ON", ignoreCase = true)

            binding.btnToggleRelay.text = if (isOn) "ON" else "OFF"
            binding.btnToggleRelay.backgroundTintList = android.content.res.ColorStateList.valueOf(
                Color.parseColor(if (isOn) "#1E88E5" else "#B0BEC5")
            )
            binding.btnToggleRelay.isEnabled = deviceOnline

            binding.btnToggleRelay.setOnClickListener {
                onToggle(field.jsonKey, if (isOn) "OFF" else "ON")
            }

            // Redupkan tampilan baris kalau device offline, biar kelihatan jelas nonaktif
            binding.root.alpha = if (deviceOnline) 1.0f else 0.5f

            binding.btnEditRelayLabel.setOnClickListener {
                onEditLabel(field.jsonKey, label)
            }
        }
    }
}
