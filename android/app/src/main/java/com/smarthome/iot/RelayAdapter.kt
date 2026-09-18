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

    fun submitStatus(device: String, item: DeviceStatusItem) {
        currentDevice = device
        currentItem = item
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
        holder.bind(field, label, value)
    }

    override fun getItemCount(): Int = RELAY_FIELDS.size

    class RelayViewHolder(
        private val binding: ItemRelayBinding,
        private val onToggle: (jsonKey: String, newStatus: String) -> Unit,
        private val onEditLabel: (jsonKey: String, currentLabel: String) -> Unit
    ) : RecyclerView.ViewHolder(binding.root) {

        fun bind(field: RelayField, label: String, value: String) {
            binding.tvRelayLabel.text = label

            val isOn = value.equals("ON", ignoreCase = true)
            binding.relayDot.setColorFilter(
                Color.parseColor(if (isOn) "#4CAF50" else "#BDBDBD")
            )

            // Lepas listener dulu sebelum set programatik, supaya tidak memicu onToggle.
            binding.switchRelay.setOnCheckedChangeListener(null)
            binding.switchRelay.isChecked = isOn
            binding.switchRelay.setOnCheckedChangeListener { _, checked ->
                onToggle(field.jsonKey, if (checked) "ON" else "OFF")
            }

            binding.btnEditRelayLabel.setOnClickListener {
                onEditLabel(field.jsonKey, label)
            }
        }
    }
}
