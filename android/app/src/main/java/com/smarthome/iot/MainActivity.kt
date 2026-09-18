package com.smarthome.iot

import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.widget.AdapterView
import android.widget.ArrayAdapter
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import androidx.recyclerview.widget.LinearLayoutManager
import com.smarthome.iot.databinding.ActivityMainBinding
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch

class MainActivity : AppCompatActivity() {

    private lateinit var binding: ActivityMainBinding
    private val mainScope = CoroutineScope(Dispatchers.Main)
    private val handler = Handler(Looper.getMainLooper())

    private lateinit var relayAdapter: RelayAdapter

    private var deviceNames: List<String> = emptyList()
    private var selectedDevice: String? = null
    private var isUpdating = false

    // Auto-refresh tiap 5 detik, supaya tampilan tetap sinkron.
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

        relayAdapter = RelayAdapter { jsonKey, newStatus ->
            updateRelay(jsonKey, newStatus)
        }
        binding.relayRecyclerView.layoutManager = LinearLayoutManager(this)
        binding.relayRecyclerView.adapter = relayAdapter

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

    /** Ambil status terbaru (5 relay + 3 suhu) untuk device yang sedang dipilih. */
    private fun loadStatus(showSpinner: Boolean) {
        val device = selectedDevice ?: return
        if (isUpdating) return
        if (showSpinner) binding.progressBar.visibility = android.view.View.VISIBLE

        mainScope.launch {
            try {
                val response = RetrofitClient.apiService.getStatus(device)
                if (response.isSuccessful) {
                    binding.tvConnectionInfo.text = "Terhubung ke server"
                    response.body()?.let { bindStatus(it) }
                }
            } catch (e: Exception) {
                binding.tvConnectionInfo.text = "Tidak terhubung ke server"
            } finally {
                binding.progressBar.visibility = android.view.View.GONE
                binding.swipeRefresh.isRefreshing = false
            }
        }
    }

    /** Update 1 relay saja -- field lain otomatis dipertahankan oleh backend. */
    private fun updateRelay(jsonKey: String, newStatus: String) {
        val device = selectedDevice ?: return
        isUpdating = true
        binding.progressBar.visibility = android.view.View.VISIBLE

        mainScope.launch {
            try {
                val body = mapOf("device" to device, jsonKey to newStatus)
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
        binding.tvLastUpdate.text = "Update terakhir: ${item.time}"
        binding.tvSuhu1.text = "${item.suhu}\u00B0C"
        binding.tvSuhu2.text = "${item.suhu2}\u00B0C"
        binding.tvSuhu3.text = "${item.suhu3}\u00B0C"
        relayAdapter.submitStatus(item)
    }
}
