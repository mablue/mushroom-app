package com.mushroom.android

import android.app.Application
import android.app.NotificationChannel
import android.app.NotificationManager
import android.os.Build

/**
 * Mushroom Application class.
 * Initializes the notification channel for the foreground service
 * and sets up the native engine configuration.
 */
class MushroomApp : Application() {

    companion object {
        const val NOTIFICATION_CHANNEL_ID = "mushroom_engine"
        const val NOTIFICATION_ID = 1001

        /** Internal storage path for the RootFS */
        lateinit var rootFsPath: String
            private set

        /** Internal storage path for the native engine working directory */
        lateinit var enginePath: String
            private set
    }

    override fun onCreate() {
        super.onCreate()
        instance = this
        rootFsPath = "${filesDir.absolutePath}/rootfs"
        enginePath = "${filesDir.absolutePath}/engine"
        createNotificationChannel()
    }

    private fun createNotificationChannel() {
        val channel = NotificationChannel(
            NOTIFICATION_CHANNEL_ID,
            getString(R.string.channel_name),
            NotificationManager.IMPORTANCE_LOW
        ).apply {
            description = getString(R.string.channel_desc)
            setShowBadge(false)
        }
        val manager = getSystemService(NotificationManager::class.java)
        manager.createNotificationChannel(channel)
    }

    override fun onTerminate() {
        super.onTerminate()
    }

    companion object {
        lateinit var instance: MushroomApp
            private set
    }
}