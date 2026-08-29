package com.mushroom.android.views

import android.content.Context
import android.graphics.Typeface
import android.os.Handler
import android.os.Looper
import android.text.method.ScrollingMovementMethod
import android.util.AttributeSet
import android.util.Log
import android.view.KeyEvent
import android.view.inputmethod.EditorInfo
import android.widget.EditText
import androidx.core.widget.doAfterTextChanged
import java.io.IOException
import java.io.InputStream
import java.io.OutputStream
import kotlin.concurrent.thread

class TerminalView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null
) : EditText(context, attrs) {
    
    companion object {
        private const val TAG = "Mushroom/Terminal"
        private const val BUFFER_SIZE = 4096
    }
    
    private var shellProcess: Process? = null
    private var inputStream: InputStream? = null
    private var outputStream: OutputStream? = null
    private val handler = Handler(Looper.getMainLooper())
    
    init {
        setupTerminal()
    }
    
    private fun setupTerminal() {
        typeface = Typeface.MONOSPACE
        textSize = 14f
        background = null
        setTextColor(0xFF00FF00.toInt())
        backgroundColor = 0xFF000000.toInt()
        movementMethod = ScrollingMovementMethod()
        
        imeOptions = EditorInfo.IME_FLAG_NO_EXTRACT_UI
        softKeyboardControlMode = SOFTWARE_KEYBOARD_CONTROL_MODE_HIDDEN
        
        doAfterTextChanged { editable ->
            val text = editable.toString()
            if (text.isNotEmpty()) {
                sendInput(text + "\n")
                setText("")
            }
        }
        
        setOnEditorActionListener { v, actionId, event ->
            if (actionId == EditorInfo.IME_ACTION_SEND || 
                (event != null && event.keyCode == KeyEvent.KEYCODE_ENTER && event.action == KeyEvent.ACTION_DOWN)) {
                val text = text.toString()
                sendInput(text + "\n")
                setText("")
                true
            } else {
                false
            }
        }
        
        startShell()
    }
    
    private fun startShell() {
        thread(name = "terminal-shell") {
            try {
                // Create a simple shell process inside the chroot
                val rootfsPath = "/data/data/com.mushroom.android/files/rootfs"
                val args = arrayOf(
                    "/bin/sh", "-i"
                )
                
                val builder = ProcessBuilder(*args)
                builder.directory(android.os.Environment.getExternalStorageDirectory())
                builder.environment().put("HOME", "/root")
                builder.environment().put("TERM", "linux")
                builder.environment().put("PATH", "/usr/bin:/bin:/usr/sbin:/sbin")
                builder.environment().put("LD_PRELOAD", "/data/data/com.mushroom.android/lib/libmushroom-preload.so")
                
                shellProcess = builder.start()
                inputStream = shellProcess?.inputStream
                outputStream = shellProcess?.outputStream
                
                Log.i(TAG, "Shell started")
                
                // Start reading output in background
                readOutput()
                
            } catch (e: IOException) {
                Log.e(TAG, "Failed to start shell", e)
                handler.post { append("Error starting shell: ${e.message}\n") }
            }
        }
    }
    
    private fun readOutput() {
        thread(name = "terminal-reader") {
            val buffer = ByteArray(BUFFER_SIZE)
            
            try {
                inputStream?.use { input ->
                    while (!Thread.currentThread().isInterrupted) {
                        val bytes = input.read(buffer)
                        if (bytes > 0) {
                            val output = String(buffer, 0, bytes)
                            handler.post {
                                append(output)
                                movementMethod?.onKeyDown(this, KeyEvent.KEYCODE_UNKNOWN, null)
                            }
                        } else if (bytes == 0) {
                            Thread.sleep(50)
                        } else {
                            break
                        }
                    }
                }
            } catch (e: IOException) {
                Log.e(TAG, "Error reading terminal output", e)
            }
        }
    }
    
    private fun sendInput(data: String) {
        handler.post {
            outputStream?.write(data.toByteArray())
            outputStream?.flush()
        }
    }
    
    override fun onDetachedFromWindow() {
        super.onDetachedFromWindow()
        shellProcess?.destroy()
    }
}
