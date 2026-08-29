package com.mushroom.android

import android.content.Intent
import android.os.Bundle
import android.util.Log
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.View
import android.widget.*
import androidx.appcompat.app.AppCompatActivity
import com.google.android.material.bottomnavigation.BottomNavigationView
import com.mushroom.android.views.LinuxDesktopView
import com.mushroom.android.views.TerminalView

/**
 * Main entry point for the Mushroom app.
 *
 * Hosts the Linux desktop SurfaceView, the built-in terminal emulator,
 * and the overlay control panel (Start/Stop, resolution picker, memory slider).
 *
 * Key responsibilities:
 * - Toggle between Desktop and Terminal views via bottom navigation
 * - Manage the LinuxService lifecycle (start/stop)
 * - Forward touch/keyboard events to the native X11 compositor
 * - Display status and FPS information
 */
class MainActivity : AppCompatActivity(),
    LinuxService.ServiceCallback,
    LinuxDesktopView.InputForwarder {

    companion object {
        private const val TAG = "MushroomActivity"
    }

    private lateinit var desktopView: LinuxDesktopView
    private lateinit var terminalView: TerminalView
    private lateinit var bottomNavigation: BottomNavigationView
    private lateinit var overlayControls: View
    private lateinit var btnStartStop: Button
    private lateinit var resolutionSpinner: Spinner
    private lateinit var memorySlider: SeekBar
    private lateinit var memoryLabel: TextView
    private lateinit var statusText: TextView
    private lateinit var statusProgress: ProgressBar

    private var isDesktopMode = false
    private var engineRunning = false

    // Resolution configuration
    private val resolutions = arrayOf(
        Triple("800x600", 800, 600),
        Triple("1024x768", 1024, 768),
        Triple("1280x720", 1280, 720)
    )
    private var selectedResolution = resolutions[1] // default 1024x768
    private var selectedMemoryMb = 2048

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        // Initialize views
        desktopView = findViewById(R.id.desktopView)
        terminalView = findViewById(R.id.terminalView)
        bottomNavigation = findViewById(R.id.bottomNavigation)
        overlayControls = findViewById(R.id.overlayControls)
        btnStartStop = findViewById(R.id.btnStartStop)
        resolutionSpinner = findViewById(R.id.resolutionSpinner)
        memorySlider = findViewById(R.id.memorySlider)
        memoryLabel = findViewById(R.id.memoryLabel)
        statusText = findViewById(R.id.statusText)
        statusProgress = findViewById(R.id.statusProgress)

        // Set up the desktop view input forwarder
        desktopView.setInputForwarder(this)

        // Set up terminal view
        terminalView.setOnTerminalReadyListener {
            Log.i(TAG, "Terminal emulator ready")
        }

        // Set up bottom navigation
        bottomNavigation.setOnItemSelectedListener { item ->
            when (item.itemId) {
                R.id.nav_desktop -> {
                    if (engineRunning) {
                        switchToDesktop()
                    }
                    true
                }
                R.id.nav_terminal -> {
                    switchToTerminal()
                    true
                }
                else -> false
            }
        }

        // Set up Start/Stop button
        btnStartStop.setOnClickListener {
            if (engineRunning) {
                stopEngine()
            } else {
                startEngine()
            }
        }

        // Set up resolution spinner
        val resolutionAdapter = ArrayAdapter(
            this,
            android.R.layout.simple_spinner_item,
            resolutions.map { it.first }
        )
        resolutionAdapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item)
        resolutionSpinner.adapter = resolutionAdapter
        resolutionSpinner.setSelection(1) // default 1024x768
        resolutionSpinner.onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
            override fun onItemSelected(parent: AdapterView<*>?, view: View?, pos: Int, id: Long) {
                selectedResolution = resolutions[pos]
                if (engineRunning) {
                    updateResolution()
                }
            }
            override fun onNothingSelected(parent: AdapterView<*>?) {}
        }

        // Set up memory slider
        memorySlider.setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
            override fun onProgressChanged(seekBar: SeekBar?, progress: Int, fromUser: Boolean) {
                // progress 0..8 → 512..4096 MB in 512MB steps
                selectedMemoryMb = (progress + 1) * 512
                memoryLabel.text = "${selectedMemoryMb / 1024}G"
            }
            override fun onStartTrackingTouch(seekBar: SeekBar?) {}
            override fun onStopTrackingTouch(seekBar: SeekBar?) {
                if (engineRunning) {
                    updateMemoryLimit()
                }
            }
        })

        // Set the service callback
        LinuxService.setCallback(this)

        // Show overlay controls
        overlayControls.visibility = View.VISIBLE

        // Start in terminal mode by default
        switchToTerminal()
    }

    override fun onResume() {
        super.onResume()
        // Re-attach surface if the engine is running
        if (engineRunning && isDesktopMode) {
            desktopView.onResume()
        }
    }

    override fun onPause() {
        super.onPause()
        if (engineRunning && isDesktopMode) {
            desktopView.onPause()
        }
    }

    override fun onDestroy() {
        LinuxService.setCallback(null)
        super.onDestroy()
    }

    // ---------- Mode switching ----------

    private fun switchToDesktop() {
        isDesktopMode = true
        desktopView.visibility = View.VISIBLE
        terminalView.visibility = View.GONE

        if (engineRunning) {
            desktopView.onResume()
        }
    }

    private fun switchToTerminal() {
        isDesktopMode = false
        desktopView.visibility = View.GONE
        terminalView.visibility = View.VISIBLE
        desktopView.onPause()

        if (engineRunning) {
            terminalView.focusTerminal()
        }
    }

    // ---------- Engine lifecycle ----------

    private fun startEngine() {
        Log.i(TAG, "Starting engine: ${selectedResolution.second}x${selectedResolution.third}, ${selectedMemoryMb}MB")

        setStatus("Starting…", true)
        btnStartStop.isEnabled = false

        val intent = Intent(this, LinuxService::class.java).apply {
            action = LinuxService.ACTION_START
            putExtra(LinuxService.EXTRA_WIDTH, selectedResolution.second)
            putExtra(LinuxService.EXTRA_HEIGHT, selectedResolution.third)
            putExtra(LinuxService.EXTRA_MEMORY_MB, selectedMemoryMb)
        }
        startService(intent)
    }

    private fun stopEngine() {
        Log.i(TAG, "Stopping engine")

        val intent = Intent(this, LinuxService::class.java).apply {
            action = LinuxService.ACTION_STOP
        }
        startService(intent)
        engineRunning = false
        updateButtonState()
        setStatus("Stopped", false)
    }

    private fun updateResolution() {
        val intent = Intent(this, LinuxService::class.java).apply {
            action = LinuxService.ACTION_UPDATE_RESOLUTION
            putExtra(LinuxService.EXTRA_WIDTH, selectedResolution.second)
            putExtra(LinuxService.EXTRA_HEIGHT, selectedResolution.third)
        }
        startService(intent)
    }

    private fun updateMemoryLimit() {
        val intent = Intent(this, LinuxService::class.java).apply {
            action = LinuxService.ACTION_UPDATE_MEMORY
            putExtra(LinuxService.EXTRA_MEMORY_MB, selectedMemoryMb)
        }
        startService(intent)
    }

    // ---------- UI state updates ----------

    private fun updateButtonState() {
        btnStartStop.isEnabled = true
        btnStartStop.text = if (engineRunning) getString(R.string.stop) else getString(R.string.start)
        btnStartStop.setBackgroundTintList(
            androidx.core.content.res.ResourcesCompat.getColorStateList(
                resources,
                if (engineRunning) android.R.color.holo_red_light else android.R.color.holo_green_light,
                theme
            )
        )
    }

    private fun setStatus(message: String, showProgress: Boolean = false) {
        runOnUiThread {
            statusText.text = message
            statusProgress.visibility = if (showProgress) View.VISIBLE else View.GONE
        }
    }

    // ---------- ServiceCallback implementation ----------

    override fun onStateChanged(state: NativeEngine.State) {
        runOnUiThread {
            when (state) {
                NativeEngine.State.RUNNING -> {
                    engineRunning = true
                    updateButtonState()
                    setStatus("Ready", false)

                    // Attach the desktop surface to the native renderer
                    if (isDesktopMode) {
                        desktopView.onResume()
                    }
                }
                NativeEngine.State.STOPPED -> {
                    engineRunning = false
                    updateButtonState()
                    setStatus("Stopped", false)
                    desktopView.onPause()
                }
                NativeEngine.State.ERROR -> {
                    engineRunning = false
                    updateButtonState()
                    setStatus("Error: ${NativeEngine.getLastError()}", false)
                }
                else -> {}
            }
        }
    }

    override fun onError(error: String) {
        runOnUiThread {
            setStatus("Error: $error", false)
            engineRunning = false
            updateButtonState()
            Toast.makeText(this, error, Toast.LENGTH_LONG).show()
        }
    }

    override fun onProgress(progress: Int, message: String) {
        runOnUiThread {
            statusProgress.progress = progress
            statusText.text = message
        }
    }

    override fun onFpsUpdate(fps: Float) {
        runOnUiThread {
            if (isDesktopMode) {
                statusText.text = "FPS: %.1f".format(fps)
            }
        }
    }

    // ---------- InputForwarder (LinuxDesktopView -> NativeEngine) ----------

    override fun forwardKeyEvent(keyCode: Int, isDown: Boolean): Boolean {
        if (!engineRunning) return false
        NativeEngine.sendKeyEvent(keyCode, isDown)
        return true
    }

    override fun forwardPointerMotion(x: Int, y: Int) {
        if (!engineRunning) return
        NativeEngine.sendPointerMotion(x, y)
    }

    override fun forwardPointerButton(button: Int, isDown: Boolean) {
        if (!engineRunning) return
        NativeEngine.sendPointerButton(button, isDown)
    }

    override fun forwardTouchEvent(x: Int, y: Int, pointerId: Int, action: Int) {
        if (!engineRunning) return
        NativeEngine.sendTouchEvent(x, y, pointerId, action)
    }

    // ---------- Key dispatch for terminal ----------

    override fun onKeyDown(keyCode: Int, event: KeyEvent?): Boolean {
        if (!engineRunning) return super.onKeyDown(keyCode, event)
        if (isDesktopMode) {
            NativeEngine.sendKeyEvent(keyCode, true)
            return true
        }
        return super.onKeyDown(keyCode, event)
    }

    override fun onKeyUp(keyCode: Int, event: KeyEvent?): Boolean {
        if (!engineRunning) return super.onKeyUp(keyCode, event)
        if (isDesktopMode) {
            NativeEngine.sendKeyEvent(keyCode, false)
            return true
        }
        return super.onKeyUp(keyCode, event)
    }
}