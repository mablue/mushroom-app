package com.mushroom.android

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Intent
import android.os.IBinder
import android.util.Log
import androidx.core.app.NotificationCompat
import java.io.File
import kotlin.concurrent.thread

class LinuxService : Service() {
    companion object {
        private const val TAG = "Mushroom/LinuxService"
        private const val CHANNEL_ID = "mushroom_linux_channel"
        private const val NOTIFICATION_ID = 1001
        
        var isRunning: Boolean = false
            private set
        var currentSessionId: Long = 0L
    }

    override fun onCreate() {
        super.onCreate()
        Log.d(TAG, "LinuxService created")
        createNotificationChannel()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_START -> {
                val rootfsPath = intent.getStringExtra(EXTRA_ROOTFS_PATH) ?: getDefaultRootfsPath()
                val width = intent.getIntExtra(EXTRA_WIDTH, 1024)
                val height = intent.getIntExtra(EXTRA_HEIGHT, 768)
                val memLimit = intent.getLongExtra(EXTRA_MEMORY_LIMIT, 512L * 1024 * 1024) // 512MB default
                
                startEngine(rootfsPath, width, height, memLimit)
            }
            ACTION_STOP -> {
                stopEngine()
            }
        }
        return START_NOT_STICKY
    }

    override fun onBind(intent: Intent?): IBinder? = null

    private fun startEngine(rootfsPath: String, width: Int, height: Int, memLimit: Long) {
        if (isRunning) {
            Log.w(TAG, "Engine already running")
            return
        }
        
        try {
            currentSessionId = System.nanoTime().toLong()
            NativeEngine.setResolution(width, height)
            NativeEngine.setMemoryLimit(memLimit)
            
            val started = NativeEngine.startEngine(rootfsPath, currentSessionId)
            if (started) {
                isRunning = true
                startForeground(NOTIFICATION_ID, buildNotification())
                Log.i(TAG, "Engine started successfully")
            } else {
                Log.e(TAG, "Failed to start engine")
                isRunning = false
                currentSessionId = 0L
            }
        } catch (e: Exception) {
            Log.e(TAG, "Error starting engine", e)
            isRunning = false
            currentSessionId = 0L
        }
    }

    private fun stopEngine() {
        if (!isRunning) return
        
        try {
            NativeEngine.stopEngine(currentSessionId)
            isRunning = false
            currentSessionId = 0L
            stopForeground(STOP_FOREGROUND_REMOVE)
            stopSelf()
            Log.i(TAG, "Engine stopped")
        } catch (e: Exception) {
            Log.e(TAG, "Error stopping engine", e)
        }
    }

    private fun createNotificationChannel() {
        val channel = NotificationChannel(
            CHANNEL_ID,
            "Linux Environment",
            NotificationManager.IMPORTANCE_LOW
        ).apply {
            description = "Mushroom Linux environment service notification"
        }
        
        val manager = getSystemService(NotificationManager::class.java)
        manager.createNotificationChannel(channel)
    }

    private fun buildNotification(): Notification {
        val pendingIntent = PendingIntent.getActivity(
            this,
            0,
            Intent(this, MainActivity::class.java),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT
        )
        
        return NotificationCompat.Builder(this, CHANNEL_ID)
            .setContentTitle("Mushroom Linux")
            .setContentText("Linux environment is running")
            .setSmallIcon(android.R.drawable.ic_dialog_info)
            .setContentIntent(pendingIntent)
            .setOngoing(true)
            .build()
    }

    private fun getDefaultRootfsPath(): String {
        return File(filesDir, "rootfs").absolutePath
    }

    interface Listener {
        fun onStarted()
        fun onStopped()
        fun onError(message: String)
    }
}

const val ACTION_START = "com.mushroom.START"
const val ACTION_STOP = "com.mushroom.STOP"
const val EXTRA_ROOTFS_PATH = "rootfs_path"
const val EXTRA_WIDTH = "width"
const val EXTRA_HEIGHT = "height"
const val EXTRA_MEMORY_LIMIT = "memory_limit"
