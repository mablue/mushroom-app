package com.mushroom.android

import android.graphics.SurfaceTexture
import android.util.Log
import android.view.Surface

/**
 * JNI bridge to the native C++ engine.
 * This class exposes all native engine operations to the Kotlin layer.
 *
 * The native library (libmushroom-engine.so) implements:
 * - Syscall interception via LD_PRELOAD wrappers
 * - RootFS download, verification, and extraction
 * - Virtual mount namespace creation
 * - Xvfb-based X11 framebuffer rendering
 * - Seccomp-BPF policy enforcement
 */
object NativeEngine {

    private const val TAG = "MushroomNative"

    /**
     * Engine lifecycle state
     */
    enum class State {
        UNINITIALIZED,
        INITIALIZING,
        READY,
        RUNNING,
        STOPPING,
        STOPPED,
        ERROR
    }

    var currentState: State = State.UNINITIALIZED
        private set

    // ---------- JNI native methods ----------

    /**
     * Initialize the native engine. Must be called before any other native method.
     * Sets up the interception layer, loads configuration, and prepares the engine directory.
     *
     * @param enginePath Absolute path to the engine working directory
     * @param rootFsPath Absolute path to the extracted RootFS
     * @return true if initialization succeeded
     */
    external fun nativeInit(enginePath: String, rootFsPath: String): Boolean

    /**
     * Start the Linux environment. This will:
     * - Create a new mount namespace
     * - Pivot root into the RootFS
     * - Mount virtual /proc, /sys, /dev, /tmp
     * - Apply seccomp-BPF policies
     * - Launch Xvfb and the desktop environment
     *
     * @param width Display width in pixels
     * @param height Display height in pixels
     * @param memLimitMb Memory limit in megabytes
     * @return true if the environment started successfully
     */
    external fun nativeStart(width: Int, height: Int, memLimitMb: Int): Boolean

    /**
     * Stop the Linux environment gracefully.
     */
    external fun nativeStop()

    /**
     * Attach an Android Surface to the native renderer.
     * The native engine will render X11 framebuffer content to this surface
     * via an EGL/OpenGL ES pipeline.
     *
     * @param surface The Android Surface to render into
     */
    external fun nativeAttachSurface(surface: Surface)

    /**
     * Detach the current rendering surface.
     */
    external fun nativeDetachSurface()

    /**
     * Get the native framebuffer width in pixels.
     */
    external fun nativeGetWidth(): Int

    /**
     * Get the native framebuffer height in pixels.
     */
    external fun nativeGetHeight(): Int

    /**
     * Send a keyboard event to the Linux environment.
     *
     * @param keyCode The X11 keysym or Android keycode
     * @param down true for key press, false for key release
     */
    external fun nativeSendKeyEvent(keyCode: Int, down: Boolean)

    /**
     * Send a pointer motion event (relative or absolute).
     *
     * @param x Absolute X coordinate
     * @param y Absolute Y coordinate
     */
    external fun nativeSendPointerMotion(x: Int, y: Int)

    /**
     * Send a pointer button event.
     *
     * @param button Button number (1=left, 2=middle, 3=right)
     * @param down true for press, false for release
     */
    external fun nativeSendPointerButton(button: Int, down: Boolean)

    /**
     * Send a touch event to the Linux environment.
     *
     * @param x Touch X coordinate
     * @param y Touch Y coordinate
     * @param pointerId Touch pointer ID
     * @param action Touch action (0=down, 1=up, 2=move)
     */
    external fun nativeSendTouchEvent(x: Int, y: Int, pointerId: Int, action: Int)

    /**
     * Check if the native engine is initialized and healthy.
     *
     * @return true if the engine is running without errors
     */
    external fun nativeIsHealthy(): Boolean

    /**
     * Get the last error message from the native engine.
     *
     * @return Error description string, or empty string if no error
     */
    external fun nativeGetLastError(): String

    /**
     * Get the current FPS of the X11 renderer.
     *
     * @return Frames per second
     */
    external fun nativeGetFps(): Float

    /**
     * Start a PTY session inside the chroot environment.
     * Used by the terminal emulator.
     *
     * @return PTY file descriptor, or -1 on failure
     */
    external fun nativeOpenPty(): Int

    /**
     * Write data to the PTY.
     *
     * @param fd PTY file descriptor
     * @param data Byte array to write
     * @return Number of bytes written, or -1 on error
     */
    external fun nativeWritePty(fd: Int, data: ByteArray): Int

    /**
     * Read data from the PTY.
     *
     * @param fd PTY file descriptor
     * @param maxSize Maximum number of bytes to read
     * @return Byte array of read data, or null on error
     */
    external fun nativeReadPty(fd: Int, maxSize: Int): ByteArray?

    /**
     * Close a PTY session.
     *
     * @param fd PTY file descriptor to close
     */
    external fun nativeClosePty(fd: Int)

    // ---------- Kotlin wrappers ----------

    /**
     * Initialize the engine and update the internal state.
     */
    fun initialize(enginePath: String, rootFsPath: String): Boolean {
        return try {
            currentState = State.INITIALIZING
            val result = nativeInit(enginePath, rootFsPath)
            currentState = if (result) State.READY else State.ERROR
            result
        } catch (e: UnsatisfiedLinkError) {
            Log.e(TAG, "Native library not loaded: ${e.message}")
            currentState = State.ERROR
            false
        }
    }

    /**
     * Start the Linux environment with the given parameters.
     */
    fun start(width: Int, height: Int, memLimitMb: Int): Boolean {
        if (currentState == State.UNINITIALIZED || currentState == State.ERROR) {
            Log.e(TAG, "Cannot start engine: state = $currentState")
            return false
        }
        val result = nativeStart(width, height, memLimitMb)
        currentState = if (result) State.RUNNING else State.ERROR
        return result
    }

    /**
     * Stop the Linux environment.
     */
    fun stop() {
        if (currentState == State.RUNNING) {
            currentState = State.STOPPING
            nativeStop()
            currentState = State.STOPPED
        }
    }

    /**
     * Attach a SurfaceTexture to the renderer.
     */
    fun attachSurface(surface: Surface) {
        nativeAttachSurface(surface)
    }

    /**
     * Detach the current surface.
     */
    fun detachSurface() {
        nativeDetachSurface()
    }

    /**
     * Send a key event to the Linux environment.
     */
    fun sendKeyEvent(keyCode: Int, down: Boolean) {
        nativeSendKeyEvent(keyCode, down)
    }

    /**
     * Send pointer motion to the Linux environment.
     */
    fun sendPointerMotion(x: Int, y: Int) {
        nativeSendPointerMotion(x, y)
    }

    /**
     * Send a pointer button event.
     */
    fun sendPointerButton(button: Int, down: Boolean) {
        nativeSendPointerButton(button, down)
    }

    /**
     * Send a touch event.
     */
    fun sendTouchEvent(x: Int, y: Int, pointerId: Int, action: Int) {
        nativeSendTouchEvent(x, y, pointerId, action)
    }

    /**
     * Check engine health.
     */
    fun isHealthy(): Boolean {
        return try {
            nativeIsHealthy()
        } catch (e: Exception) {
            false
        }
    }

    /**
     * Get the last error message.
     */
    fun getLastError(): String {
        return try {
            nativeGetLastError()
        } catch (e: Exception) {
            e.message ?: "Unknown error"
        }
    }

    /**
     * Get current render FPS.
     */
    fun getFps(): Float {
        return try {
            nativeGetFps()
        } catch (e: Exception) {
            0f
        }
    }

    /**
     * Open a PTY session inside the chroot.
     */
    fun openPty(): Int {
        return nativeOpenPty()
    }

    /**
     * Write data to a PTY session.
     */
    fun writePty(fd: Int, data: ByteArray): Int {
        return nativeWritePty(fd, data)
    }

    /**
     * Read data from a PTY session.
     */
    fun readPty(fd: Int, maxSize: Int): ByteArray? {
        return nativeReadPty(fd, maxSize)
    }

    /**
     * Close a PTY session.
     */
    fun closePty(fd: Int) {
        nativeClosePty(fd)
    }

    companion object {
        private var isLoaded = false

        /**
         * Load the native library. Called once during application startup.
         * Must be called before any native method.
         */
        fun loadLibrary() {
            if (!isLoaded) {
                try {
                    System.loadLibrary("mushroom-engine")
                    isLoaded = true
                    Log.i(TAG, "Native library loaded successfully")
                } catch (e: UnsatisfiedLinkError) {
                    Log.e(TAG, "Failed to load native library: ${e.message}")
                    throw e
                }
            }
        }
    }
}