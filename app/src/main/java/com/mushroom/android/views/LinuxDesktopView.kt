package com.mushroom.android.views

import android.content.Context
import android.graphics.SurfaceTexture
import android.opengl.EGLConfig
import android.opengl.EGLContext
import android.opengl.EGLDisplay
import android.opengl.EGLExt
import android.opengl.EGL14
import android.opengl.GLES20
import android.opengl.GLSurfaceView
import android.util.AttributeSet
import android.util.Log
import android.view.MotionEvent
import android.view.Surface
import android.view.TextureView
import javax.microedition.khronos.egl.EGLConfig
import javax.microedition.khronos.opengles.GL10
import kotlin.math.min

class LinuxDesktopView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null
) : GLSurfaceView(context, attrs), TextureView.SurfaceTextureListener {
    
    companion object {
        private const val TAG = "Mushroom/DesktopView"
        private const val FPS = 30
        private const val FRAMEBUFFER_FORMAT = GLES20.GL_RGB
        private const val FRAMEBUFFER_TYPE = GLES20.GL_UNSIGNED_BYTE
    }
    
    private var surfaceTexture: SurfaceTexture? = null
    private var nativeSurface: Surface? = null
    private var frameBufferTextureId = -1
    
    private var framebufferWidth = 0
    private var framebufferHeight = 0
    private var framebufferStride = 0
    
    private val renderer = DesktopRenderer()
    
    init {
        setEGLContextClientVersion(2)
        setRenderer(this)
        renderMode = RENDERMODE_WHEN_DIRTY
        isOpaque = false
    }
    
    override fun onSurfaceTextureAvailable(st: SurfaceTexture, w: Int, h: Int) {
        surfaceTexture = st
        nativeSurface = Surface(st)
        
        // Attach surface to native engine
        com.mushroom.android.NativeEngine.attachSurface(nativeSurface!!)
        Log.d(TAG, "Surface attached: ${w}x${h}")
        
        requestRender()
    }
    
    override fun onSurfaceTextureSizeChanged(st: SurfaceTexture, w: Int, h: Int) {
        Log.d(TAG, "Surface size changed: ${w}x${h}")
    }
    
    override fun onSurfaceTextureDestroyed(st: SurfaceTexture): Boolean {
        com.mushroom.android.NativeEngine.detachSurface()
        nativeSurface?.release()
        nativeSurface = null
        surfaceTexture = null
        return true
    }
    
    override fun onSurfaceTextureUpdated(st: SurfaceTexture) {
        requestRender()
    }
    
    override fun onTouchEvent(event: MotionEvent): Boolean {
        if (event.action == MotionEvent.ACTION_DOWN) {
            val x = event.x
            val y = event.y
            val screenWidth = width.toFloat()
            val screenHeight = height.toFloat()
            
            // Convert touch coordinates to normalized device coordinates for X11
            val nx = (x / screenWidth) * 2.0f - 1.0f
            val ny = 1.0f - (y / screenHeight) * 2.0f
            
            com.mushroom.android.NativeEngine.sendTouchEvent(
                event.id, nx, ny, event.action
            )
            return true
        }
        return super.onTouchEvent(event)
    }
    
    override fun onDrawFrame(gl: GL10) {
        if (surfaceTexture == null) return
        
        // Read framebuffer data from native code if available
        // In a real implementation, we'd poll native buffer here
        gl.glClear(GLES20.GL_COLOR_BUFFER_BIT)
        
        // Render texture from Xvfb output
        surfaceTexture?.updateTexImage()
        
        // Upload to OpenGL texture
        if (frameBufferTextureId != -1) {
            GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, frameBufferTextureId)
            GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_MIN_FILTER, GLES20.GL_LINEAR)
            GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_MAG_FILTER, GLES20.GL_LINEAR)
        }
    }
    
    override fun onSurfaceChanged(gl: GL10, w: Int, h: Int) {
        gl.glViewport(0, 0, w, h)
    }
    
    override fun onSurfaceCreated(gl: GL10, config: EGLConfig) {
        // Create texture for framebuffer
        val textures = IntArray(1)
        GLES20.glGenTextures(1, textures, 0)
        frameBufferTextureId = textures[0]
        
        GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, frameBufferTextureId)
        GLES20.glTexParameterf(GLES20.GL_TEXTURE_2D, 
            GLES20.GL_TEXTURE_MIN_FILTER, GLES20.GL_LINEAR.toFloat())
        GLES20.glTexParameterf(GLES20.GL_TEXTURE_2D, 
            GLES20.GL_TEXTURE_MAG_FILTER, GLES20.GL_LINEAR.toFloat())
        GLES20.glTexParameterf(GLES20.GL_TEXTURE_2D,
            GLES20.GL_TEXTURE_WRAP_S, GLES20.GL_CLAMP_TO_EDGE.toFloat())
        GLES20.glTexParameterf(GLES20.GL_TEXTURE_2D,
            GLES20.GL_TEXTURE_WRAP_T, GLES20.GL_CLAMP_TO_EDGE.toFloat())
    }
}
