package com.mushroom.android

import android.graphics.SurfaceTexture
import android.util.Log
import android.view.Surface

object NativeEngine {
    private const val TAG = "Mushroom/NativeEngine"
    
    init {
        System.loadLibrary("mushroom-engine")
    }

    @JvmStatic external fun attachSurface(surface: Surface)
    @JvmStatic external fun detachSurface()
    @JvmStatic external fun startEngine(rootfsPath: String, sessionId: Long): Boolean
    @JvmStatic external fun stopEngine(sessionId: Long): Boolean
    @JvmStatic external fun setResolution(width: Int, height: Int)
    @JvmStatic external fun setMemoryLimit(bytes: Long)
    @JvmStatic external fun updateFramebuffer(texId: Int, width: Int, height: Int, stride: Int)
    @JvmStatic external fun sendKeyEvent(keyCode: Int, isKeyDown: Boolean)
    @JvmStatic external fun sendTouchEvent(eventId: Int, x: Float, y: Float, action: Int)
    
    data class EngineState(
        val isActive: Boolean,
        val rootfsPath: String,
        val resolutionWidth: Int,
        val resolutionHeight: Int,
        val memoryLimitBytes: Long
    )
}
