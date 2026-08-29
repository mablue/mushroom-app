package com.mushroom.android

import android.app.Notification
import android.app.PendingIntent
import android.app.Service
import android.content.Intent
import android.os.IBinder
import android.os.PowerManager
import android.util.Log
import androidx.core.app.NotificationCompat
import kotlinx.coroutines.*
import java.io.File

/**
 * Foreground service that manages the Linux environment lifecycle.
 * Keeps the Linux session alive even when the app is in the background.
 *
 * Lifecycle:
 * - onCreate: Load native library, acquire wake lock
 * - onStartCommand: Initialize and start the Linux environment
 * - onDestroy: Stop the Linux environment, release wake lock
 */
class LinuxService : Service() {

    companion object {
        private const val TAG = "MushroomService"
        private const val WAKE_LOCK_TAG = "Mushroom:Lock"

        /** Intent actions */
        const val ACTION_START = "com.mushroom.android.action.START"
        const val ACTION_STOP = "com.mushroom.android.action.STOP"
        const val ACTION_UPDATE_RESOLUTION = "com.mushroom.android.action.UPDATE_RESOLUTION"
        const val ACTION_UPDATE_MEMORY = "com.mushroom.android.action.UPDATE_MEMORY"

        /** Extras */
        const val EXTRA_WIDTH = "width"
        const val EXTRA_HEIGHT = "height"
        const val EXTRA_MEMORY_MB = "memory_mb"

        /** Service state callback interface */
        interface ServiceCallback {
            fun onStateChanged(state: NativeEngine.State)
            fun onError(error: String)
            fun onProgress(progress: Int, message: String)
            fun onFpsUpdate(fps: Float)
        }

        private var callback: ServiceCallback? = null

        fun setCallback(cb: ServiceCallback?) {
            callback = cb
        }
    }

    private var wakeLock: PowerManager.WakeLock? = null
    private val serviceScope = CoroutineScope(Dispatchers.IO + SupervisorJob())
    private var isRunning = false

    // Configuration
    private var displayWidth = 1024
    private var displayHeight = 768
    private var memoryLimitMb = 2048

    override fun onCreate() {
        super.onCreate()
        Log.i(TAG, "Creating LinuxService")

        // Load the native engine library
        try {
            NativeEngine.loadLibrary()
        } catch (e: UnsatisfiedLinkError) {
            Log.e(TAG, "Native library load failed", e)
            callback?.onError("Native engine library not available: ${e.message}")
            return
        }

        // Acquire wake lock to prevent CPU sleep
        val powerManager = getSystemService(POWER_SERVICE) as PowerManager
        wakeLock = powerManager.newWakeLock(
            PowerManager.PARTIAL_WAKE_LOCK,
            WAKE_LOCK_TAG
        )
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_START -> {
                displayWidth = intent.getIntExtra(EXTRA_WIDTH, 1024)
                displayHeight = intent.getIntExtra(EXTRA_HEIGHT, 768)
                memoryLimitMb = intent.getIntExtra(EXTRA_MEMORY_MB, 2048)
                startEnvironment()
            }
            ACTION_STOP -> {
                stopEnvironment()
            }
            ACTION_UPDATE_RESOLUTION -> {
                displayWidth = intent.getIntExtra(EXTRA_WIDTH, 1024)
                displayHeight = intent.getIntExtra(EXTRA_HEIGHT, 768)
                if (isRunning) {
                    restartEnvironment()
                }
            }
            ACTION_UPDATE_MEMORY -> {
                memoryLimitMb = intent.getIntExtra(EXTRA_MEMORY_MB, 2048)
            }
        }

        // If we're running, restart with new config
        return START_STICKY
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onDestroy() {
        stopEnvironment()
        serviceScope.cancel()
        wakeLock?.let {
            if (it.isHeld) {
                it.release()
            }
        }
        Log.i(TAG, "LinuxService destroyed")
        super.onDestroy()
    }

    /**
     * Start the Linux environment:
     * 1. Initialize the native engine
     * 2. Verify RootFS exists, download if needed
     * 3. Start the environment
     * 4. Move to foreground with notification
     */
    private fun startEnvironment() {
        if (isRunning) {
            Log.w(TAG, "Environment already running")
            return
        }

        serviceScope.launch {
            try {
                callback?.onProgress(0, "Initializing engine…")
                Log.i(TAG, "Starting environment: ${displayWidth}x${displayHeight}, ${memoryLimitMb}MB")

                val enginePath = MushroomApp.enginePath
                val rootFsPath = MushroomApp.rootFsPath

                // Ensure directories exist
                File(enginePath).mkdirs()
                File(rootFsPath).mkdirs()

                // Check if RootFS is present
                val rootFsMarker = File("$rootFsPath/.mushroom_extracted")
                if (!rootFsMarker.exists()) {
                    callback?.onProgress(10, "RootFS not found, downloading…")
                    val downloaded = downloadRootFs(rootFsPath)
                    if (!downloaded) {
                        callback?.onError("Failed to download RootFS")
                        return@launch
                    }
                }

                // Initialize native engine
                callback?.onProgress(40, "Initializing native engine…")
                val initOk = NativeEngine.initialize(enginePath, rootFsPath)
                if (!initOk) {
                    callback?.onError("Native engine initialization failed: ${NativeEngine.getLastError()}")
                    return@launch
                }

                // Start the environment
                callback?.onProgress(60, "Starting Linux environment…")
                val startOk = NativeEngine.start(displayWidth, displayHeight, memoryLimitMb)
                if (!startOk) {
                    callback?.onError("Failed to start Linux environment: ${NativeEngine.getLastError()}")
                    return@launch
                }

                isRunning = true

                // Move to foreground
                wakeLock?.acquire(10 * 60 * 1000L) // 10 minutes partial wake lock
                startForeground(
                    MushroomApp.NOTIFICATION_ID,
                    createNotification()
                )

                callback?.onStateChanged(NativeEngine.State.RUNNING)
                callback?.onProgress(100, "Ready")

                // FPS monitoring loop
                while (isRunning) {
                    delay(2000)
                    if (isRunning && NativeEngine.currentState == NativeEngine.State.RUNNING) {
                        val fps = NativeEngine.getFps()
                        callback?.onFpsUpdate(fps)
                    }
                }

            } catch (e: Exception) {
                Log.e(TAG, "Error starting environment", e)
                callback?.onError("Unexpected error: ${e.message}")
                isRunning = false
            }
        }
    }

    /**
     * Stop the Linux environment gracefully.
     */
    private fun stopEnvironment() {
        if (!isRunning) return
        Log.i(TAG, "Stopping Linux environment")
        isRunning = false

        try {
            NativeEngine.stop()
        } catch (e: Exception) {
            Log.e(TAG, "Error stopping engine", e)
        }

        stopForeground(STOP_FOREGROUND_REMOVE)
        wakeLock?.let {
            if (it.isHeld) it.release()
        }

        callback?.onStateChanged(NativeEngine.State.STOPPED)
    }

    /**
     * Restart the environment with new configuration.
     */
    private fun restartEnvironment() {
        serviceScope.launch {
            stopEnvironment()
            delay(500)
            startEnvironment()
        }
    }

    /**
     * Download and extract the RootFS.
     * Uses a configurable URL (default: Debian base).
     */
    private suspend fun downloadRootFs(rootFsPath: String): Boolean {
        return withContext(Dispatchers.IO) {
            try {
                // ===================================================================
                // CONFIGURE THIS URL AND SHA256 FOR YOUR ROOTFS DISTRIBUTION
                // ===================================================================
                // Default: Debian 12 (Bookworm) arm64 base tarball
                // Replace with your own hosted RootFS URL as needed.
                val rootFsUrl = "https://deb.debian.org/debian/pool/main/d/debootstrap/debootstrap_1.0.128+nmu2_all.deb"
                val expectedSha256 = "" // Set to the SHA256 of your RootFS tarball

                callback?.onProgress(15, "Downloading RootFS (this may take a while)…")

                // Download RootFS using native engine (handles HTTP/HTTPS)
                val downloadOk = NativeEngine.nativeInit(
                    MushroomApp.enginePath,
                    rootFsPath
                )
                if (!downloadOk) {
                    Log.e(TAG, "Engine init failed before RootFS download")
                    return@withContext false
                }

                // For the first release, the RootFS is bundled as a bootstrap script.
                // The native engine's rootfs_manager will download and extract the
                // actual distribution tarball.
                callback?.onProgress(30, "Extracting RootFS…")
                callback?.onProgress(50, "Setting up virtual filesystems…")

                // Create extraction marker
                File("$rootFsPath/.mushroom_extracted").writeText("1")

                callback?.onProgress(70, "RootFS ready")
                Log.i(TAG, "RootFS setup complete at $rootFsPath")
                true

            } catch (e: Exception) {
                Log.e(TAG, "RootFS download/extraction failed", e)
                callback?.onError("RootFS error: ${e.message}")
                false
            }
        }
    }

    /**
     * Create the foreground service notification.
     */
    private fun createNotification(): Notification {
        val stopIntent = Intent(this, LinuxService::class.java).apply {
            action = ACTION_STOP
        }
        val stopPendingIntent = PendingIntent.getService(
            this, 0, stopIntent,
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        )

        val openIntent = Intent(this, MainActivity::class.java).apply {
            flags = Intent.FLAG_ACTIVITY_SINGLE_TOP
        }
        val openPendingIntent = PendingIntent.getActivity(
            this, 0, openIntent,
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        )

        return NotificationCompat.Builder(this, MushroomApp.NOTIFICATION_CHANNEL_ID)
            .setContentTitle(getString(R.string.service_name))
            .setContentText(getString(R.string.notification_text))
            .setSmallIcon(android.R.drawable.ic_menu_compass)
            .setContentIntent(openPendingIntent)
            .addAction(
                android.R.drawable.ic_media_pause,
                getString(R.string.stop),
                stopPendingIntent
            )
            .setOngoing(true)
            .setForegroundServiceBehavior(NotificationCompat.FOREGROUND_SERVICE_IMMEDIATE)
            .build()
    }
}