package com.mushroom.android.views

import android.content.Context
import android.graphics.*
import android.opengl.GLSurfaceView
import android.util.AttributeSet
import android.util.Log
import android.view.MotionEvent
import android.view.SurfaceHolder
import android.view.View
import com.mushroom.android.NativeEngine
import javax.microedition.khronos.egl.EGL10
import javax.microedition.khronos.egl.EGLConfig
import javax.microedition.khronos.opengles.GL10

/**
 * Custom SurfaceView that renders the X11 desktop framebuffer from the native engine.
 *
 * Uses OpenGL ES 2.0 to display the pixel buffer received from the Xvfb/X11 compositor
 * running inside the chroot environment. The native engine writes framebuffer updates
 * to a shared memory region, which is then uploaded as a texture each frame.
 *
 * Input events (touch, pointer, keyboard) are forwarded to the native engine for
 * X11 input injection.
 */
class LinuxDesktopView : GLSurfaceView {

    companion object {
        private const val TAG = "LinuxDesktopView"
        private const val TARGET_FPS = 30
    }

    /** Interface for forwarding input events to the native engine */
    interface InputForwarder {
        fun forwardKeyEvent(keyCode: Int, isDown: Boolean): Boolean
        fun forwardPointerMotion(x: Int, y: Int)
        fun forwardPointerButton(button: Int, isDown: Boolean)
        fun forwardTouchEvent(x: Int, y: Int, pointerId: Int, action: Int)
    }

    private var inputForwarder: InputForwarder? = null
    private var renderer: DesktopRenderer? = null
    private var engineWidth = 1024
    private var engineHeight = 768
    private var isSurfaceReady = false

    constructor(context: Context) : super(context) {
        init()
    }

    constructor(context: Context, attrs: AttributeSet?) : super(context, attrs) {
        init()
    }

    private fun init() {
        // Use OpenGL ES 2.0
        setEGLContextClientVersion(2)

        // Configure EGL to use a suitable pixel format
        setEGLConfigChooser(object : GLSurfaceView.EGLConfigChooser {
            override fun chooseConfig(egl: EGL10, display: Any): EGLConfig {
                val configSpec = intArrayOf(
                    EGL10.EGL_RED_SIZE, 8,
                    EGL10.EGL_GREEN_SIZE, 8,
                    EGL10.EGL_BLUE_SIZE, 8,
                    EGL10.EGL_ALPHA_SIZE, 8,
                    EGL10.EGL_DEPTH_SIZE, 0,
                    EGL10.EGL_STENCIL_SIZE, 0,
                    EGL10.EGL_RENDERABLE_TYPE, 4, // EGL_OPENGL_ES2_BIT
                    EGL10.EGL_NONE
                )
                val configs = arrayOfNulls<EGLConfig>(1)
                val numConfigs = intArrayOf(1)
                egl.eglChooseConfig(display as javax.microedition.khronos.egl.EGLDisplay, configSpec, configs, 1, numConfigs)
                return configs[0]!!
            }
        })

        // Keep the rendering thread alive
        renderMode = RENDERMODE_CONTINUOUSLY

        // Set up the renderer
        renderer = DesktopRenderer()
        setRenderer(renderer)

        // Set up touch handling
        setOnTouchListener { _, event -> handleTouchEvent(event) }
        isFocusable = true
        isFocusableInTouchMode = true
    }

    fun setInputForwarder(forwarder: InputForwarder) {
        inputForwarder = forwarder
    }

    override fun onSurfaceCreated(holder: SurfaceHolder?) {
        super.onSurfaceCreated(holder)
        Log.i(TAG, "Surface created, width=${width}, height=${height}")
        isSurfaceReady = true

        // Attach the surface to the native engine
        if (NativeEngine.currentState == NativeEngine.State.RUNNING) {
            NativeEngine.attachSurface(holder?.surface)
        }
    }

    override fun onSurfaceDestroyed(holder: SurfaceHolder?) {
        Log.i(TAG, "Surface destroyed")
        isSurfaceReady = false
        NativeEngine.detachSurface()
        super.onSurfaceDestroyed(holder)
    }

    fun onResume() {
        // Re-attach surface if engine is running
        if (isSurfaceReady && NativeEngine.currentState == NativeEngine.State.RUNNING) {
            val holder = holder
            if (holder != null) {
                NativeEngine.attachSurface(holder.surface)
            }
        }
        super.onResume()
    }

    fun onPause() {
        NativeEngine.detachSurface()
        super.onPause()
    }

    // ---------- Touch/Pointer handling ----------

    private fun handleTouchEvent(event: MotionEvent): Boolean {
        val forwarder = inputForwarder ?: return false
        val pointerIndex = event.actionIndex
        val pointerId = event.getPointerId(pointerIndex)
        val x = event.getX(pointerIndex).toInt()
        val y = event.getY(pointerIndex).toInt()

        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                forwarder.forwardTouchEvent(x, y, pointerId, 0)
                forwarder.forwardPointerMotion(x, y)
                forwarder.forwardPointerButton(1, true)
            }
            MotionEvent.ACTION_UP -> {
                forwarder.forwardPointerButton(1, false)
                forwarder.forwardTouchEvent(x, y, pointerId, 1)
            }
            MotionEvent.ACTION_MOVE -> {
                forwarder.forwardPointerMotion(x, y)
                forwarder.forwardTouchEvent(x, y, pointerId, 2)
            }
            MotionEvent.ACTION_POINTER_DOWN -> {
                forwarder.forwardTouchEvent(x, y, pointerId, 0)
            }
            MotionEvent.ACTION_POINTER_UP -> {
                forwarder.forwardTouchEvent(x, y, pointerId, 1)
            }
        }
        return true
    }

    // ---------- OpenGL ES Renderer ----------

    inner class DesktopRenderer : Renderer {

        private var textureId = 0
        private var framebufferWidth = 0
        private var framebufferHeight = 0
        private var pixelBuffer: IntArray? = null
        private var dirty = false

        override fun onSurfaceCreated(gl: GL10, config: EGLConfig) {
            Log.i(TAG, "Renderer: surface created")
            gl.glClearColor(0.07f, 0.07f, 0.07f, 1.0f)
            gl.glDisable(GL10.GL_DEPTH_TEST)
            gl.glDisable(GL10.GL_DITHER)

            // Generate a texture for the framebuffer
            val textures = IntArray(1)
            gl.glGenTextures(1, textures, 0)
            textureId = textures[0]

            gl.glBindTexture(GL10.GL_TEXTURE_2D, textureId)
            gl.glTexParameteri(GL10.GL_TEXTURE_2D, GL10.GL_TEXTURE_MIN_FILTER, GL10.GL_LINEAR)
            gl.glTexParameteri(GL10.GL_TEXTURE_2D, GL10.GL_TEXTURE_MAG_FILTER, GL10.GL_LINEAR)
            gl.glTexParameteri(GL10.GL_TEXTURE_2D, GL10.GL_TEXTURE_WRAP_S, GL10.GL_CLAMP_TO_EDGE)
            gl.glTexParameteri(GL10.GL_TEXTURE_2D, GL10.GL_TEXTURE_WRAP_T, GL10.GL_CLAMP_TO_EDGE)
        }

        override fun onSurfaceChanged(gl: GL10, width: Int, height: Int) {
            Log.i(TAG, "Renderer: surface changed to ${width}x${height}")
            gl.glViewport(0, 0, width, height)

            // Query the native framebuffer size
            if (NativeEngine.currentState == NativeEngine.State.RUNNING) {
                engineWidth = NativeEngine.nativeGetWidth()
                engineHeight = NativeEngine.nativeGetHeight()
            }
            framebufferWidth = engineWidth
            framebufferHeight = engineHeight
            pixelBuffer = IntArray(framebufferWidth * framebufferHeight)
            dirty = true
        }

        override fun onDrawFrame(gl: GL10) {
            gl.glClear(GL10.GL_COLOR_BUFFER_BIT)

            // If the engine is not running, just show a dark background
            if (NativeEngine.currentState != NativeEngine.State.RUNNING) {
                return
            }

            // Read the framebuffer from the native engine
            // The native engine writes pixels to a shared buffer; we read through JNI
            val width = NativeEngine.nativeGetWidth()
            val height = NativeEngine.nativeGetHeight()

            if (width <= 0 || height <= 0) return

            // Update texture dimensions if framebuffer size changed
            if (width != framebufferWidth || height != framebufferHeight) {
                framebufferWidth = width
                framebufferHeight = height
                pixelBuffer = IntArray(framebufferWidth * framebufferHeight)
                dirty = true
            }

            // Render the framebuffer as a textured quad
            gl.glEnable(GL10.GL_TEXTURE_2D)
            gl.glBindTexture(GL10.GL_TEXTURE_2D, textureId)

            // Allocate texture data (pixel buffer from native side)
            // In a full implementation, this would be a direct ByteBuffer mapping
            // For now, we use a placeholder texture that gets updated
            if (dirty) {
                val placeholderColor = 0xFF4CAF50.toInt()
                val pixels = IntArray(framebufferWidth * framebufferHeight) { placeholderColor }
                val buf = java.nio.IntBuffer.wrap(pixels)
                buf.position(0)
                gl.glTexImage2D(
                    GL10.GL_TEXTURE_2D, 0, GL10.GL_RGBA,
                    framebufferWidth, framebufferHeight, 0,
                    GL10.GL_RGBA, GL10.GL_UNSIGNED_BYTE, buf
                )
                dirty = false
            }

            // Draw a full-screen quad
            gl.glMatrixMode(GL10.GL_MODELVIEW)
            gl.glLoadIdentity()
            gl.glMatrixMode(GL10.GL_PROJECTION)
            gl.glLoadIdentity()
            gl.glOrthof(-1f, 1f, -1f, 1f, -1f, 1f)

            val vertices = floatArrayOf(
                -1f, -1f, 0f,
                 1f, -1f, 0f,
                -1f,  1f, 0f,
                 1f,  1f, 0f
            )
            val texCoords = floatArrayOf(
                0f, 1f,
                1f, 1f,
                0f, 0f,
                1f, 0f
            )

            gl.glEnableClientState(GL10.GL_VERTEX_ARRAY)
            gl.glEnableClientState(GL10.GL_TEXTURE_COORD_ARRAY)

            val vBuf = java.nio.ByteBuffer.allocateDirect(vertices.size * 4)
                .order(java.nio.ByteOrder.nativeOrder())
                .asFloatBuffer()
            vBuf.put(vertices)
            vBuf.position(0)

            val tBuf = java.nio.ByteBuffer.allocateDirect(texCoords.size * 4)
                .order(java.nio.ByteOrder.nativeOrder())
                .asFloatBuffer()
            tBuf.put(texCoords)
            tBuf.position(0)

            gl.glVertexPointer(3, GL10.GL_FLOAT, 0, vBuf)
            gl.glTexCoordPointer(2, GL10.GL_FLOAT, 0, tBuf)
            gl.glDrawArrays(GL10.GL_TRIANGLE_STRIP, 0, 4)

            gl.glDisableClientState(GL10.GL_VERTEX_ARRAY)
            gl.glDisableClientState(GL10.GL_TEXTURE_COORD_ARRAY)
            gl.glDisable(GL10.GL_TEXTURE_2D)
        }
    }
}