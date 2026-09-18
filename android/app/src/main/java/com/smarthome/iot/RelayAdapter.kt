package com.smarthome.iot

import android.graphics.Color
import android.view.LayoutInflater
import android.view.ViewGroup
import androidx.recyclerview.widget.RecyclerView
import com.smarthome.iot.databinding.ItemRelayBinding

class RelayAdapter(
    private val onToggle: (jsonKey: String, newStatus: String) -> Unit
) : RecyclerView.Adapter<RelayAdapter.RelayViewHolder>() {

    private var currentItem: DeviceStatusItem? = null

    fun submitStatus(item: DeviceStatusItem) {
        currentItem = item
        notifyItemRangeChanged(0, RELAY_FIELDS.size)
    }

    override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): RelayViewHolder {
        val binding = ItemRelayBinding.inflate(LayoutInflater.from(parent.context), parent, false)
        return RelayViewHolder(binding, onToggle)
    }

    override fun onBindViewHolder(holder: RelayViewHolder, position: Int) {
        val field = RELAY_FIELDS[position]
        val value = currentItem?.valueFor(field.jsonKey) ?: "OFF"
        holder.bind(field, value)
    }

    override fun getItemCount(): Int = RELAY_FIELDS.size

    class RelayViewHolder(
        private val binding: ItemRelayBinding,
        private val onToggle: (jsonKey: String, newStatus: String) -> Unit
    ) : RecyclerView.ViewHolder(binding.root) {

        fun bind(field: RelayField, value: String) {
            binding.tvRelayLabel.text = field.label

            val isOn = value.equals("ON", ignoreCase = true)
            binding.tvRelayStatus.text = if (isOn) "ON" else "OFF"
            binding.relayDot.setColorFilter(
                Color.parseColor(if (isOn) "#4CAF50" else "#F44336")
            )

            binding.btnRelayOn.setOnClickListener { onToggle(field.jsonKey, "ON") }
            binding.btnRelayOff.setOnClickListener { onToggle(field.jsonKey, "OFF") }
        }
    }
}
