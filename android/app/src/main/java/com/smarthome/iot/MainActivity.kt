package com.smarthome.iot

import android.graphics.Color
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.widget.AdapterView
import android.widget.ArrayAdapter
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import com.smarthome.iot.databinding.ActivityMainBinding
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch

class MainActivity : AppCompatActivity() {

    private lateinit var binding: ActivityMainBinding
    private val mainScope = CoroutineScope(Dispatchers.Main)
    private val handler = Handler(Looper.getMainLooper())

    private var deviceNames: List<String> = emptyList()
    private var selectedDevice: String? = null
    private var isUpdating = false

    // Auto-refresh label status tiap 5 detik, supaya sinkron kalau device
    // diubah dari tempat lain (mis. langsung lewat API/tombol fisik).
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

        binding.swipeRefresh.setOnRefreshListener {
            loadDeviceListThenStatus()
        }

        binding.deviceSpinner.onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
            override fun onItemSelected(parent: AdapterView<*>?, view: android.view.View?, position: Int, id: Long) {
                val device = deviceNames.getOrNull(position) ?: return
                if (device != selectedDevice) {
                    selectedDevice = device
                    loadStatus(showSpinner = true)
                }
            }

            override fun onNothingSelected(parent: AdapterView<*>?) {}
        }

        binding.btnOn.setOnClickListener { updateStatus("ON") }
        binding.btnOff.setOnClickListener { updateStatus("OFF") }

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

    /** Ambil daftar device dari GET /api/status buat isi combo box. */
    private fun loadDeviceListThenStatus() {
        binding.progressBar.visibility = android.view.View.VISIBLE
        mainScope.launch {
            try {
                val response = RetrofitClient.apiService.getAllStatus()
                if (response.isSuccessful) {
                    binding.tvConnectionInfo.text = "Terhubung ke server"

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

                    loadStatus(showSpinner = false)
                } else {
                    binding.tvConnectionInfo.text = "Error server (${response.code()})"
                    binding.progressBar.visibility = android.view.View.GONE
                    binding.swipeRefresh.isRefreshing = false
                }
            } catch (e: Exception) {
                binding.tvConnectionInfo.text = "Tidak terhubung ke server"
                Toast.makeText(this@MainActivity, "Gagal konek: ${e.message}", Toast.LENGTH_SHORT).show()
                binding.progressBar.visibility = android.view.View.GONE
                binding.swipeRefresh.isRefreshing = false
            }
        }
    }

    /** Ambil status terbaru untuk device yang sedang dipilih. */
    private fun loadStatus(showSpinner: Boolean) {
        val device = selectedDevice ?: return
        if (isUpdating) return // jangan timpa tampilan pas lagi proses klik tombol
        if (showSpinner) binding.progressBar.visibility = android.view.View.VISIBLE

        mainScope.launch {
            try {
                val response = RetrofitClient.apiService.getStatus(device)
                if (response.isSuccessful) {
                    binding.tvConnectionInfo.text = "Terhubung ke server"
                    response.body()?.let { bindStatus(it) }
                } else if (response.code() == 404) {
                    bindStatus(DeviceStatusItem(device, "OFF", "-"))
                }
            } catch (e: Exception) {
                binding.tvConnectionInfo.text = "Tidak terhubung ke server"
            } finally {
                binding.progressBar.visibility = android.view.View.GONE
                binding.swipeRefresh.isRefreshing = false
            }
        }
    }

    /** Kirim perintah ON/OFF ke server (dibaca ESP8266 lewat polling). */
    private fun updateStatus(status: String) {
        val device = selectedDevice ?: return
        isUpdating = true
        binding.progressBar.visibility = android.view.View.VISIBLE
        binding.btnOn.isEnabled = false
        binding.btnOff.isEnabled = false

        mainScope.launch {
            try {
                val response = RetrofitClient.apiService.updateStatus(UpdateStatusRequest(device, status))
                if (response.isSuccessful) {
                    response.body()?.data?.let { bindStatus(it) }
                    Toast.makeText(this@MainActivity, "Status $device diubah ke $status", Toast.LENGTH_SHORT).show()
                } else {
                    Toast.makeText(this@MainActivity, "Gagal update (${response.code()})", Toast.LENGTH_SHORT).show()
                }
            } catch (e: Exception) {
                Toast.makeText(this@MainActivity, "Gagal konek: ${e.message}", Toast.LENGTH_SHORT).show()
            } finally {
                isUpdating = false
                binding.progressBar.visibility = android.view.View.GONE
                binding.btnOn.isEnabled = true
                binding.btnOff.isEnabled = true
            }
        }
    }

    private fun bindStatus(item: DeviceStatusItem) {
        binding.tvLastUpdate.text = "Update terakhir: ${item.time}"

        if (item.status.equals("ON", ignoreCase = true)) {
            binding.tvStatusLabel.text = "ON"
            binding.tvStatusLabel.setTextColor(Color.parseColor("#2E7D32"))
            binding.statusDot.setColorFilter(Color.parseColor("#4CAF50"))
        } else {
            binding.tvStatusLabel.text = "OFF"
            binding.tvStatusLabel.setTextColor(Color.parseColor("#D32F2F"))
            binding.statusDot.setColorFilter(Color.parseColor("#F44336"))
        }
    }
}
