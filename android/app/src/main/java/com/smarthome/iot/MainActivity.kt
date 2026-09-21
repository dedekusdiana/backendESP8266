package com.smarthome.iot

import android.app.AlertDialog
import android.graphics.Color
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.text.InputType
import android.widget.AdapterView
import android.widget.ArrayAdapter
import android.widget.EditText
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import androidx.recyclerview.widget.LinearLayoutManager
import com.smarthome.iot.databinding.ActivityMainBinding
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch

class MainActivity : AppCompatActivity() {

    private lateinit var binding: ActivityMainBinding
    private lateinit var labelPrefs: LabelPrefs
    private val mainScope = CoroutineScope(Dispatchers.Main)
    private val handler = Handler(Looper.getMainLooper())

    private lateinit var relayAdapter: RelayAdapter

    private var deviceNames: List<String> = emptyList()
    private var selectedDevice: String? = null
    private var isUpdating = false

    private val pollIntervalMs = 5_000L
    private val pollRunnable = object : Runnable {
        override fun run() {
            loadStatus(showSpinner = false)
            handler.postDelayed(this, pollIntervalMs)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)

        labelPrefs = LabelPrefs(this)

        relayAdapter = RelayAdapter(
            labelPrefs = labelPrefs,
            onToggle = { jsonKey, newStatus -> updateField(jsonKey, newStatus) },
            onEditLabel = { jsonKey, currentLabel -> showRenameDialog(currentLabel) { newLabel ->
                selectedDevice?.let { labelPrefs.setLabel(it, jsonKey, newLabel) }
                relayAdapter.refreshLabels()
            } }
        )
        binding.relayRecyclerView.layoutManager = LinearLayoutManager(this)
        binding.relayRecyclerView.adapter = relayAdapter

        setupSuhuCard(binding.cardSuhu1.tvSuhuTitle, binding.cardSuhu1.btnEditSuhuTitle, SUHU_FIELDS[0].jsonKey)
        setupSuhuCard(binding.cardSuhu2.tvSuhuTitle, binding.cardSuhu2.btnEditSuhuTitle, SUHU_FIELDS[1].jsonKey)
        setupSuhuCard(binding.cardSuhu3.tvSuhuTitle, binding.cardSuhu3.btnEditSuhuTitle, SUHU_FIELDS[2].jsonKey)

        binding.swipeRefresh.setOnRefreshListener {
            loadDeviceListThenStatus()
        }

        binding.deviceSpinner.onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
            override fun onItemSelected(parent: AdapterView<*>?, view: android.view.View?, position: Int, id: Long) {
                val device = deviceNames.getOrNull(position) ?: return
                if (device != selectedDevice) {
                    selectedDevice = device
                    refreshSuhuTitles()
                    loadStatus(showSpinner = true)
                }
            }

            override fun onNothingSelected(parent: AdapterView<*>?) {}
        }

        loadDeviceListThenStatus()
    }

    override fun onResume() {
        super.onResume()
        handler.postDelayed(pollRunnable, pollIntervalMs)
    }

    override fun onPause() {
        super.onPause()
        handler.removeCallbacks(pollRunnable)
    }

    private fun setupSuhuCard(
        titleView: android.widget.TextView,
        editButton: android.widget.ImageButton,
        jsonKey: String
    ) {
        editButton.setOnClickListener {
            showRenameDialog(titleView.text.toString()) { newLabel ->
                selectedDevice?.let { labelPrefs.setLabel(it, jsonKey, newLabel) }
                titleView.text = newLabel
            }
        }
    }

    private fun refreshSuhuTitles() {
        val device = selectedDevice ?: return
        binding.cardSuhu1.tvSuhuTitle.text = labelPrefs.getLabel(device, SUHU_FIELDS[0].jsonKey, SUHU_FIELDS[0].label)
        binding.cardSuhu2.tvSuhuTitle.text = labelPrefs.getLabel(device, SUHU_FIELDS[1].jsonKey, SUHU_FIELDS[1].label)
        binding.cardSuhu3.tvSuhuTitle.text = labelPrefs.getLabel(device, SUHU_FIELDS[2].jsonKey, SUHU_FIELDS[2].label)
    }

    private fun showRenameDialog(currentLabel: String, onSave: (String) -> Unit) {
        val input = EditText(this).apply {
            inputType = InputType.TYPE_CLASS_TEXT
            setText(currentLabel)
            setSelection(currentLabel.length)
        }

        AlertDialog.Builder(this)
            .setTitle("Ubah nama")
            .setView(input)
            .setPositiveButton("Simpan") { _, _ ->
                val newLabel = input.text.toString().trim()
                if (newLabel.isNotEmpty()) onSave(newLabel)
            }
            .setNegativeButton("Batal", null)
            .show()
    }

    private fun setDatabaseStatus(connected: Boolean, detail: String? = null) {
        binding.tvDatabaseStatus.text = if (connected) "Connected" else "Terputus"
        binding.tvDatabaseStatus.setTextColor(
            Color.parseColor(if (connected) "#2E7D32" else "#D32F2F")
        )
        if (detail != null) binding.tvConnectionInfo.text = detail
    }

    private fun loadDeviceListThenStatus() {
        binding.progressBar.visibility = android.view.View.VISIBLE
        mainScope.launch {
            try {
                val response = RetrofitClient.apiService.getAllStatus()
                if (response.isSuccessful) {
                    setDatabaseStatus(true, "Terhubung ke server")

                    val devices = response.body()?.devices.orEmpty()
                    deviceNames = devices.map { it.device }

                    if (deviceNames.isEmpty()) {
                        Toast.makeText(this@MainActivity, "Belum ada device di database.", Toast.LENGTH_LONG).show()
                        binding.progressBar.visibility = android.view.View.GONE
                        binding.swipeRefresh.isRefreshing = false
                        return@launch
                    }

                    val adapter = ArrayAdapter(
                        this@MainActivity,
                        android.R.layout.simple_spinner_item,
                        deviceNames
                    )
                    adapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item)
                    binding.deviceSpinner.adapter = adapter

                    val keepIndex = deviceNames.indexOf(selectedDevice).let { if (it >= 0) it else 0 }
                    selectedDevice = deviceNames[keepIndex]
                    binding.deviceSpinner.setSelection(keepIndex)
                    refreshSuhuTitles()

                    loadStatus(showSpinner = false)
                } else {
                    setDatabaseStatus(false, "Error server (${response.code()})")
                    binding.progressBar.visibility = android.view.View.GONE
                    binding.swipeRefresh.isRefreshing = false
                }
            } catch (e: Exception) {
                setDatabaseStatus(false, "Tidak terhubung ke server")
                Toast.makeText(this@MainActivity, "Gagal konek: ${e.message}", Toast.LENGTH_SHORT).show()
                binding.progressBar.visibility = android.view.View.GONE
                binding.swipeRefresh.isRefreshing = false
            }
        }
    }

    private fun loadStatus(showSpinner: Boolean) {
        val device = selectedDevice ?: return
        if (isUpdating) return
        if (showSpinner) binding.progressBar.visibility = android.view.View.VISIBLE

        mainScope.launch {
            try {
                val response = RetrofitClient.apiService.getStatus(device)
                if (response.isSuccessful) {
                    setDatabaseStatus(true, "Terhubung ke server")
                    response.body()?.let { bindStatus(it) }
                }
            } catch (e: Exception) {
                setDatabaseStatus(false, "Tidak terhubung ke server")
            } finally {
                binding.progressBar.visibility = android.view.View.GONE
                binding.swipeRefresh.isRefreshing = false
            }
        }
    }

    private fun updateField(jsonKey: String, newValue: String) {
        val device = selectedDevice ?: return
        isUpdating = true
        binding.progressBar.visibility = android.view.View.VISIBLE

        mainScope.launch {
            try {
                val body = mapOf("device" to device, jsonKey to newValue)
                val response = RetrofitClient.apiService.updateStatus(body)
                if (response.isSuccessful) {
                    response.body()?.data?.let { bindStatus(it) }
                } else {
                    Toast.makeText(this@MainActivity, "Gagal update (${response.code()})", Toast.LENGTH_SHORT).show()
                }
            } catch (e: Exception) {
                Toast.makeText(this@MainActivity, "Gagal konek: ${e.message}", Toast.LENGTH_SHORT).show()
            } finally {
                isUpdating = false
                binding.progressBar.visibility = android.view.View.GONE
            }
        }
    }

    private fun bindStatus(item: DeviceStatusItem) {
        val device = selectedDevice ?: item.device

        binding.tvLastUpdate.text = "Update terakhir: ${TimeUtils.toJakartaTime(item.time)}"

        binding.cardSuhu1.tvSuhuValue.text = "${item.valueForSuhu("suhu")}\u00B0C"
        binding.cardSuhu2.tvSuhuValue.text = "${item.valueForSuhu("suhu2")}\u00B0C"
        binding.cardSuhu3.tvSuhuValue.text = "${item.valueForSuhu("suhu3")}\u00B0C"

        val isOnline = item.statusDevice.equals("online", ignoreCase = true)
        binding.tvDeviceStatus.text = if (isOnline) "Online" else "Offline"
        binding.tvDeviceStatus.setTextColor(
            Color.parseColor(if (isOnline) "#2E7D32" else "#D32F2F")
        )

        relayAdapter.submitStatus(device, item)
    }
}
