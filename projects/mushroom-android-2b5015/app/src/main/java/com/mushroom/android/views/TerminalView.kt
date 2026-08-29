package com.mushroom.android.views

import android.content.Context
import android.graphics.*
import android.util.AttributeSet
import android.util.Log
import android.view.KeyEvent
import android.view.View
import android.view.inputmethod.BaseInputConnection
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.InputConnection
import android.view.inputmethod.InputMethodManager
import com.mushroom.android.NativeEngine
import java.io.ByteArrayOutputStream
import java.nio.charset.StandardCharsets
import java.util.concurrent.atomic.AtomicBoolean
import kotlin.concurrent.thread

/**
 * Built-in terminal emulator view that connects to a PTY session inside the
 * chroot Linux environment. Uses a lightweight VT100 parser for rendering.
 *
 * This replaces the need for an external VNC viewer or separate terminal app.
 * The terminal communicates with `/bin/sh` (or the user's default shell)
 * running inside the Linux sandbox via a PTY file descriptor.
 */
class TerminalView : View {

    companion object {
        private const val TAG = "TerminalView"
        private const val FONT_SIZE_DP = 14f
        private const val COLS_DEFAULT = 80
        private const val ROWS_DEFAULT = 24
        private const val SCROLLBACK_LINES = 2000
        private const val POLL_INTERVAL_MS = 20L
        private const val CURSOR_BLINK_MS = 500L
    }

    /** Callback when the terminal emulator is ready for I/O */
    interface OnTerminalReadyListener {
        fun onTerminalReady()
    }

    private var onTerminalReadyListener: OnTerminalReadyListener? = null

    fun setOnTerminalReadyListener(listener: OnTerminalReadyListener?) {
        onTerminalReadyListener = listener
    }

    // Terminal state
    private var ptyFd: Int = -1
    private val isRunning = AtomicBoolean(false)
    private var cursorRow = 0
    private var cursorCol = 0
    private var cursorVisible = true
    private var blinkToggle = true

    // Screen buffer: rows x cols of characters + attributes
    private data class CharCell(
        var char: Char = ' ',
        var fgColor: Int = Color.WHITE,
        var bgColor: Int = Color.TRANSPARENT,
        var bold: Boolean = false
    )

    private val screenBuffer = mutableListOf<MutableList<CharCell>>()
    private val scrollbackBuffer = mutableListOf<MutableList<CharCell>>()
    private var cols = COLS_DEFAULT
    private var rows = ROWS_DEFAULT

    // Rendering
    private val paint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.WHITE
        textSize = FONT_SIZE_DP * resources.displayMetrics.scaledDensity
        typeface = Typeface.MONOSPACE
    }
    private val bgPaint = Paint().apply {
        color = Color.argb(200, 0, 0, 0)
    }
    private val cursorPaint = Paint().apply {
        color = Color.argb(180, 76, 175, 80)
    }
    private val selectionPaint = Paint().apply {
        color = Color.argb(80, 76, 175, 80)
    }

    private var charWidth = 0f
    private var charHeight = 0f
    private var terminalWidth = 0
    private var terminalHeight = 0

    // Input
    private val inputBuffer = ByteArrayOutputStream()
    private var inputConnection: InputConnection? = null
    private var inputQueue = mutableListOf<Byte>()

    // Cursor blink
    private var lastBlinkTime = System.currentTimeMillis()
    private var lastPollTime = 0L

    constructor(context: Context) : super(context) {
        init()
    }

    constructor(context: Context, attrs: AttributeSet?) : super(context, attrs) {
        init()
    }

    private fun init() {
        isFocusable = true
        isFocusableInTouchMode = true
        setBackgroundColor(Color.BLACK)

        // Initialize screen buffer
        resetScreen()

        // Start the PTY polling thread
        startPollingThread()

        // Start cursor blink
        post(object : Runnable {
            override fun run() {
                val now = System.currentTimeMillis()
                if (now - lastBlinkTime > CURSOR_BLINK_MS) {
                    blinkToggle = !blinkToggle
                    lastBlinkTime = now
                    postInvalidate()
                }
                postDelayed(this, CURSOR_BLINK_MS / 2)
            }
        })
    }

    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) {
        super.onSizeChanged(w, h, oldw, oldh)
        calculateDimensions()
        resizeScreen()
    }

    private fun calculateDimensions() {
        charWidth = paint.measureText("W")
        val fm = paint.fontMetrics
        charHeight = fm.descent - fm.ascent

        terminalWidth = width
        terminalHeight = height

        cols = (terminalWidth / charWidth).toInt().coerceAtLeast(20)
        rows = (terminalHeight / charHeight).toInt().coerceAtLeast(5)
    }

    private fun resetScreen() {
        screenBuffer.clear()
        for (r in 0 until rows) {
            val row = mutableListOf<CharCell>()
            for (c in 0 until cols) {
                row.add(CharCell())
            }
            screenBuffer.add(row)
        }
        cursorRow = 0
        cursorCol = 0
    }

    private fun resizeScreen() {
        val currentRows = screenBuffer.size
        val currentCols = if (currentRows > 0) screenBuffer[0].size else cols

        // Expand or shrink rows
        while (screenBuffer.size < rows) {
            val row = mutableListOf<CharCell>()
            for (c in 0 until cols) {
                row.add(CharCell())
            }
            screenBuffer.add(row)
        }
        while (screenBuffer.size > rows) {
            scrollbackBuffer.add(screenBuffer.removeAt(0))
            if (scrollbackBuffer.size > SCROLLBACK_LINES) {
                scrollbackBuffer.removeAt(0)
            }
        }

        // Adjust column widths
        for (r in 0 until screenBuffer.size) {
            val row = screenBuffer[r]
            while (row.size < cols) {
                row.add(CharCell())
            }
            while (row.size > cols) {
                row.removeAt(row.size - 1)
            }
        }
    }

    // ---------- PTY I/O ----------

    /**
     * Open a PTY session connected to the shell inside the chroot.
     * Called when the Linux environment is ready.
     */
    fun connectPty() {
        if (ptyFd >= 0) {
            disconnectPty()
        }

        ptyFd = NativeEngine.openPty()
        if (ptyFd < 0) {
            Log.e(TAG, "Failed to open PTY")
            return
        }

        isRunning.set(true)
        Log.i(TAG, "PTY connected, fd=$ptyFd")
        onTerminalReadyListener?.onTerminalReady()
    }

    /**
     * Disconnect the PTY session.
     */
    fun disconnectPty() {
        isRunning.set(false)
        if (ptyFd >= 0) {
            NativeEngine.closePty(ptyFd)
            ptyFd = -1
        }
    }

    /**
     * Focus the terminal and show the keyboard.
     */
    fun focusTerminal() {
        requestFocus()
        val imm = context.getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager
        imm.showSoftInput(this, InputMethodManager.SHOW_IMPLICIT)
    }

    /**
     * Write a string to the PTY.
     */
    fun writeToPty(text: String) {
        if (ptyFd < 0) return
        val data = text.toByteArray(StandardCharsets.UTF_8)
        NativeEngine.writePty(ptyFd, data)
    }

    /**
     * Write a byte to the PTY.
     */
    fun writeByte(b: Byte) {
        if (ptyFd < 0) return
        NativeEngine.writePty(ptyFd, byteArrayOf(b))
    }

    // ---------- Polling thread ----------

    private fun startPollingThread() {
        thread(name = "terminal-poll", isDaemon = true) {
            while (true) {
                try {
                    if (isRunning.get() && ptyFd >= 0) {
                        val data = NativeEngine.readPty(ptyFd, 4096)
                        if (data != null && data.isNotEmpty()) {
                            processInput(data)
                            postInvalidate()
                        }
                    }
                    Thread.sleep(POLL_INTERVAL_MS)
                } catch (e: Exception) {
                    Log.e(TAG, "PTY poll error", e)
                }
            }
        }
    }

    // ---------- VT100/ANSI parser ----------

    private val ansiBuffer = StringBuilder()
    private var inEscape = false
    private var inOsc = false
    private var oscBuffer = StringBuilder()

    private fun processInput(data: ByteArray) {
        for (b in data) {
            val c = b.toInt().toChar()

            if (inOsc) {
                // Operating System Command (OSC)
                if (c == '\u0007' || c == '\u001b') {
                    inOsc = false
                    handleOsc(oscBuffer.toString())
                    oscBuffer = StringBuilder()
                } else {
                    oscBuffer.append(c)
                }
                continue
            }

            if (inEscape) {
                if (c == '[') {
                    // CSI sequence
                    ansiBuffer.clear()
                    continue
                } else if (c == ']') {
                    // OSC sequence
                    inOsc = true
                    oscBuffer = StringBuilder()
                    inEscape = false
                    continue
                } else {
                    // Single-character escape sequence
                    inEscape = false
                    handleEscape(c)
                }
                continue
            }

            if (c == '\u001b') {
                inEscape = true
                ansiBuffer.clear()
                continue
            }

            if (ansiBuffer.isNotEmpty() || c == '[') {
                // Collecting CSI parameters
                if (c == '[' && ansiBuffer.isEmpty()) {
                    ansiBuffer.append(c)
                    continue
                }
                ansiBuffer.append(c)

                if (c in 'A'..'Z' || c in 'a'..'z') {
                    // End of CSI sequence
                    val seq = ansiBuffer.toString()
                    ansiBuffer.clear()
                    inEscape = false
                    handleCsi(seq)
                }
                continue
            }

            // Regular character
            handleChar(c)
        }
    }

    private fun handleEscape(c: Char) {
        when (c) {
            'c' -> resetScreen() // RIS
            'M' -> scrollUp()    // Reverse Index
            'D' -> scrollDown()  // Index
            'E' -> { cursorRow++; cursorCol = 0 } // Next Line
        }
    }

    private fun handleCsi(seq: String) {
        // Remove the leading '[' and trailing command letter
        val params = seq.drop(1).dropLast(1).split(';')
        val cmd = seq.last()

        when (cmd) {
            'A' -> { // Cursor Up
                val n = params.firstOrNull()?.toIntOrNull() ?: 1
                cursorRow = (cursorRow - n).coerceAtLeast(0)
            }
            'B' -> { // Cursor Down
                val n = params.firstOrNull()?.toIntOrNull() ?: 1
                cursorRow = (cursorRow + n).coerceAtMost(screenBuffer.size - 1)
            }
            'C' -> { // Cursor Forward
                val n = params.firstOrNull()?.toIntOrNull() ?: 1
                cursorCol = (cursorCol + n).coerceAtMost(cols - 1)
            }
            'D' -> { // Cursor Back
                val n = params.firstOrNull()?.toIntOrNull() ?: 1
                cursorCol = (cursorCol - n).coerceAtLeast(0)
            }
            'H', 'f' -> { // Cursor Position
                val r = params.getOrNull(0)?.toIntOrNull()?.minus(1) ?: 0
                val c = params.getOrNull(1)?.toIntOrNull()?.minus(1) ?: 0
                cursorRow = r.coerceIn(0, screenBuffer.size - 1)
                cursorCol = c.coerceIn(0, cols - 1)
            }
            'J' -> { // Erase in Display
                val mode = params.firstOrNull()?.toIntOrNull() ?: 0
                when (mode) {
                    0 -> { // Erase from cursor to end of screen
                        for (r in cursorRow until screenBuffer.size) {
                            val startCol = if (r == cursorRow) cursorCol else 0
                            for (c in startCol until cols) {
                                screenBuffer[r][c] = CharCell()
                            }
                        }
                    }
                    2 -> { // Erase entire screen
                        for (r in 0 until screenBuffer.size) {
                            for (c in 0 until cols) {
                                screenBuffer[r][c] = CharCell()
                            }
                        }
                    }
                }
            }
            'K' -> { // Erase in Line
                val mode = params.firstOrNull()?.toIntOrNull() ?: 0
                when (mode) {
                    0 -> { // Erase from cursor to end of line
                        for (c in cursorCol until cols) {
                            screenBuffer[cursorRow][c] = CharCell()
                        }
                    }
                    2 -> { // Erase entire line
                        for (c in 0 until cols) {
                            screenBuffer[cursorRow][c] = CharCell()
                        }
                    }
                }
            }
            'L' -> { // Insert Lines
                val n = params.firstOrNull()?.toIntOrNull() ?: 1
                repeat(n) { scrollDown() }
            }
            'M' -> { // Delete Lines
                val n = params.firstOrNull()?.toIntOrNull() ?: 1
                repeat(n) { scrollUp() }
            }
            'P' -> { // Delete Characters
                val n = (params.firstOrNull()?.toIntOrNull() ?: 1).coerceAtMost(cols - cursorCol)
                for (c in cursorCol until cols - n) {
                    screenBuffer[cursorRow][c] = screenBuffer[cursorRow][c + n]
                }
                for (c in (cols - n) until cols) {
                    screenBuffer[cursorRow][c] = CharCell()
                }
            }
            'm' -> { // SGR - Select Graphic Rendition
                // Handle color codes (simplified - just parse common ones)
                handleSgr(params)
            }
            'r' -> { // Set Scrolling Region
                // Not implemented - ignore
            }
            's' -> { // Save Cursor
                // Not implemented
            }
            'u' -> { // Restore Cursor
                // Not implemented
            }
            'h' -> { // Set Mode
                // Not implemented
            }
            'l' -> { // Reset Mode
                // Not implemented
            }
        }
    }

    private fun handleSgr(params: List<String>) {
        if (params.isEmpty() || params[0].isEmpty() || params[0] == "0") {
            // Reset attributes
            return
        }
        // Parse color codes (basic support)
        for (p in params) {
            val code = p.toIntOrNull() ?: continue
            when {
                code == 1 -> {} // Bold - mark for future use
                code in 30..37 -> {} // Foreground color
                code in 40..47 -> {} // Background color
            }
        }
    }

    private fun handleOsc(osc: String) {
        // Handle OSC sequences (e.g., set window title)
        Log.d(TAG, "OSC: $osc")
    }

    private fun handleChar(c: Char) {
        when (c) {
            '\n' -> {
                // Line feed - move cursor down, scroll if needed
                cursorRow++
                if (cursorRow >= screenBuffer.size) {
                    scrollUp()
                    cursorRow = screenBuffer.size - 1
                }
            }
            '\r' -> {
                // Carriage return
                cursorCol = 0
            }
            '\b' -> {
                // Backspace
                if (cursorCol > 0) cursorCol--
            }
            '\t' -> {
                // Tab
                val nextTab = ((cursorCol / 8) + 1) * 8
                cursorCol = nextTab.coerceAtMost(cols - 1)
            }
            '\u0007' -> {
                // Bell - ignore
            }
            else -> {
                if (c.code >= 32) {
                    // Printable character
                    if (cursorRow < screenBuffer.size && cursorCol < cols) {
                        screenBuffer[cursorRow][cursorCol] = CharCell(char = c)
                        cursorCol++
                        if (cursorCol >= cols) {
                            cursorCol = 0
                            cursorRow++
                            if (cursorRow >= screenBuffer.size) {
                                scrollUp()
                                cursorRow = screenBuffer.size - 1
                            }
                        }
                    }
                }
            }
        }
    }

    private fun scrollUp() {
        // Move the top row to scrollback and shift everything up
        if (screenBuffer.isNotEmpty()) {
            scrollbackBuffer.add(screenBuffer.removeAt(0))
            if (scrollbackBuffer.size > SCROLLBACK_LINES) {
                scrollbackBuffer.removeAt(0)
            }
            // Add a new blank row at the bottom
            val newRow = mutableListOf<CharCell>()
            for (c in 0 until cols) {
                newRow.add(CharCell())
            }
            screenBuffer.add(newRow)
        }
    }

    private fun scrollDown() {
        // Insert a blank row at the top and shift everything down
        if (screenBuffer.isNotEmpty()) {
            screenBuffer.removeAt(screenBuffer.size - 1)
            val newRow = mutableListOf<CharCell>()
            for (c in 0 until cols) {
                newRow.add(CharCell())
            }
            screenBuffer.add(0, newRow)
        }
    }

    // ---------- Rendering ----------

    override fun onDraw(canvas: Canvas) {
        super.onDraw(canvas)

        // Background
        canvas.drawRect(0f, 0f, width.toFloat(), height.toFloat(), bgPaint)

        // Ensure screen buffer matches current dimensions
        if (screenBuffer.size != rows || (screenBuffer.isNotEmpty() && screenBuffer[0].size != cols)) {
            resizeScreen()
        }

        // Draw each character cell
        for (r in 0 until screenBuffer.size.coerceAtMost(rows)) {
            val row = screenBuffer[r]
            val y = r * charHeight + paint.textSize

            for (c in 0 until row.size.coerceAtMost(cols)) {
                val cell = row[c]
                val x = c * charWidth

                // Draw background
                if (cell.bgColor != Color.TRANSPARENT) {
                    val bg = Paint().apply { color = cell.bgColor }
                    canvas.drawRect(x, y - paint.textSize, x + charWidth, y, bg)
                }

                // Draw character
                if (cell.char != ' ') {
                    paint.color = cell.fgColor
                    paint.isFakeBoldText = cell.bold
                    canvas.drawText(cell.char.toString(), x, y, paint)
                    paint.isFakeBoldText = false
                }
            }
        }

        // Draw cursor
        if (cursorVisible && blinkToggle && cursorRow < rows && cursorCol < cols) {
            val cx = cursorCol * charWidth
            val cy = cursorRow * charHeight
            val cursorHeight = paint.textSize
            canvas.drawRect(cx, cy, cx + charWidth, cy + cursorHeight, cursorPaint)
        }
    }

    // ---------- Input handling ----------

    override fun onCreateInputConnection(outAttrs: EditorInfo): InputConnection {
        outAttrs.actionLabel = null
        outAttrs.inputType = EditorInfo.TYPE_CLASS_TEXT
        outAttrs.imeOptions = EditorInfo.IME_FLAG_NO_EXTRACT_UI

        inputConnection = object : BaseInputConnection(this, true) {
            override fun commitText(text: CharSequence, newCursorPosition: Int): Boolean {
                writeToPty(text.toString())
                return true
            }

            override fun deleteSurroundingText(beforeLength: Int, afterLength: Int): Boolean {
                // Send backspace
                writeByte(0x7F.toByte())
                return true
            }

            override fun sendKeyEvent(event: KeyEvent): Boolean {
                if (event.action == KeyEvent.ACTION_DOWN) {
                    val code = event.keyCode
                    when (code) {
                        KeyEvent.KEYCODE_DEL -> writeByte(0x7F.toByte())
                        KeyEvent.KEYCODE_ENTER -> writeByte('\n'.code.toByte())
                        KeyEvent.KEYCODE_TAB -> writeByte('\t'.code.toByte())
                        KeyEvent.KEYCODE_DPAD_LEFT -> writeToPty("\u001b[D")
                        KeyEvent.KEYCODE_DPAD_RIGHT -> writeToPty("\u001b[C")
                        KeyEvent.KEYCODE_DPAD_UP -> writeToPty("\u001b[A")
                        KeyEvent.KEYCODE_DPAD_DOWN -> writeToPty("\u001b[B")
                        else -> {
                            val char = event.unicodeChar
                            if (char in 0x20..0x7E) {
                                writeByte(char.toByte())
                            }
                        }
                    }
                }
                return true
            }
        }
        return inputConnection!!
    }

    override fun onKeyDown(keyCode: Int, event: KeyEvent?): Boolean {
        if (event != null && ptyFd >= 0) {
            inputConnection?.sendKeyEvent(event)
            return true
        }
        return super.onKeyDown(keyCode, event)
    }

    override fun onKeyUp(keyCode: Int, event: KeyEvent?): Boolean {
        return true
    }

    override fun onCheckIsTextEditor(): Boolean = true
}