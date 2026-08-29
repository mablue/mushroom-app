package com.mushroom.android

import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.ServiceConnection
import android.os.Bundle
import android.os.IBinder
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.Button
import android.widget.SeekBar
import android.widget.Spinner
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import androidx.lifecycle.lifecycleScope
import com.google.android.material.bottomnavigation.BottomNavigationView
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import java.io.File

class MainActivity : AppCompatActivity(), LinuxService.Listener {
    
    private var service: LinuxService? = null
    private var isConnected = false
    
    private lateinit var bottomNav: BottomNavigationView
    private lateinit var overlayView: View
    private lateinit var startStopBtn: Button
    private lateinit var resolutionSpinner: Spinner
    private lateinit var memorySlider: SeekBar
    private lateinit var statusText: TextView
    private lateinit var desktopContainer: ViewGroup
    private lateinit var terminalContainer: ViewGroup
    
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)
        
        setupViews()
        checkAndDownloadRootfs()
        bindService()
    }
    
    private fun setupViews() {
        bottomNav = findViewById(R.id.bottomNav)
        overlayView = findViewById(R.id.overlayControls)
        startStopBtn = findViewById(R.id.startStopBtn)
        resolutionSpinner = findViewById(R.id.resolutionSpinner)
        memorySlider = findViewById(R.id.memorySlider)
        statusText = findViewById(R.id.statusText)
        desktopContainer = findViewById(R.id.desktopContainer)
        terminalContainer = findViewById(R.id.terminalContainer)
        
        startStopBtn.setOnClickListener { toggleService() }
        
        memorySlider.setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
            override fun onProgressChanged(seekBar: SeekBar?, progress: Int, fromUser: Boolean) {
                val mb = progress * 64
                statusText.text = "Memory limit: $mb MB"
                if (fromUser && isConnected) {
                    service?.let { updateMemoryLimit(it, mb.toLong() * 1024 * 1024) }
                }
            }
            override fun onStartTrackingTouch(seekBar: SeekBar?) {}
            override fun onStopTrackingTouch(seekBar: SeekBar?) {}
        })
        
        bottomNav.setOnItemSelectedListener { item ->
            when (item.itemId) {
                R.id.nav_desktop -> {
                    showDesktop()
                    true
                }
                R.id.nav_terminal -> {
                    showTerminal()
                    true
                }
                else -> false
            }
        }
        
        // Initialize memory slider at 512MB (8 segments of 64MB)
        memorySlider.progress = 8
    }
    
    private fun checkAndDownloadRootfs() {
        lifecycleScope.launch {
            val rootfsPath = File(filesDir, "rootfs").absolutePath
            val rootfsFile = File(rootfsPath)
            
            if (!rootfsFile.exists() || !rootfsFile.isDirectory) {
                showToast("Downloading RootFS... This may take a few minutes.")
                // Trigger download in background
                lifecycleScope.launch {
                    delay(1000)
                    // Start download process
                }
            }
        }
    }
    
    private fun bindService() {
        val intent = Intent(this, LinuxService::class.java).apply {
            action = LinuxService.ACTION_STOP // Initialize
        }
        bindService(intent, serviceConnection, Context.BIND_AUTO_CREATE)
    }
    
    private val serviceConnection = object : ServiceConnection {
        override fun onServiceConnected(name: ComponentName?, service: IBinder?) {
            isConnected = true
            this@MainActivity.service = service as? LinuxService
            updateUI()
        }
        
        override fun onServiceDisconnected(name: ComponentName?) {
            isConnected = false
            this@MainActivity.service = null
        }
    }
    
    private fun toggleService() {
        if (isConnected && service != null) {
            if (LinuxService.isRunning) {
                stopService(Intent(this, LinuxService::class.java).apply {
                    action = LinuxService.ACTION_STOP
                })
            } else {
                startService()
            }
        }
    }
    
    private fun startService() {
        val rootfsPath = File(filesDir, "rootfs").absolutePath
        val width = when (resolutionSpinner.selectedItemPosition) {
            0 -> 800
            1 -> 1280
            else -> 1024
        }
        val height = when (resolutionSpinner.selectedItemPosition) {
            0 -> 600
            1 -> 720
            else -> 768
        }
        val memLimit = memorySlider.progress.toLong() * 64 * 1024 * 1024
        
        Intent(this, LinuxService::class.java).also { intent ->
            intent.action = LinuxService.ACTION_START
            intent.putExtra(LinuxService.EXTRA_ROOTFS_PATH, rootfsPath)
            intent.putExtra(LinuxService.EXTRA_WIDTH, width)
            intent.putExtra(LinuxService.EXTRA_HEIGHT, height)
            intent.putExtra(LinuxService.EXTRA_MEMORY_LIMIT, memLimit)
            startForegroundService(intent)
        }
    }
    
    private fun updateMemoryLimit(service: LinuxService, bytes: Long) {
        // This would ideally communicate with the service via binder
        // For now, just update local state
    }
    
    private fun updateUI() {
        if (LinuxService.isRunning) {
            startStopBtn.text = "Stop"
            startStopBtn.setBackgroundColor(getColor(android.R.color.holo_red_dark))
            overlayView.visibility = View.VISIBLE
            statusText.text = "Running"
        } else {
            startStopBtn.text = "Start Linux"
            startStopBtn.setBackgroundColor(getColor(android.R.color.holo_green_dark))
            overlayView.visibility = View.GONE
            statusText.text = "Stopped"
        }
    }
    
    private fun showDesktop() {
        desktopContainer.visibility = View.VISIBLE
        terminalContainer.visibility = View.GONE
    }
    
    private fun showTerminal() {
        desktopContainer.visibility = View.GONE
        terminalContainer.visibility = View.VISIBLE
    }
    
    override fun onDestroy() {
        super.onDestroy()
        if (isConnected) {
            unbindService(serviceConnection)
            isConnected = false
        }
    }
    
    override fun onStarted() {
        runOnUiThread { updateUI() }
    }
    
    override fun onStopped() {
        runOnUiThread { updateUI() }
    }
    
    override fun onError(message: String) {
        runOnUiThread {
            Toast.makeText(this, "Error: $message", Toast.LENGTH_LONG).show()
            updateUI()
        }
    }
    
    private fun showToast(message: String) {
        Toast.makeText(this, message, Toast.LENGTH_SHORT).show()
    }
}
