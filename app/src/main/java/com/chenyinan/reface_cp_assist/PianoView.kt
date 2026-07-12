package com.chenyinan.reface_cp_assist

import android.content.Context
import android.graphics.*
import android.util.AttributeSet
import android.view.View

class PianoView(context: Context, attrs: AttributeSet?) : View(context, attrs) {

    companion object {
        const val NOTE_START = 24   // C1
        const val NOTE_END = 96     // C7
        private const val WHITE_KEYS_PER_OCTAVE = 7
        private const val VISIBLE_WHITE_KEYS = 20f
    }

    // State — set from outside
    var pressedNotes: Set<Int> = emptySet()
        set(v) { field = v; invalidate() }
    var sustainedNotes: Set<Int> = emptySet()
        set(v) { field = v; invalidate() }

    // Key dimensions (calculated in onSizeChanged)
    private var whiteKeyW = 0f
    private var whiteKeyH = 0f
    private var blackKeyW = 0f
    private var blackKeyH = 0f

    // Paints
    private val whitePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.rgb(240, 240, 240)
    }
    private val whitePressedPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.rgb(200, 210, 255)
    }
    private val blackPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.rgb(30, 30, 30)
    }
    private val blackPressedPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.rgb(60, 60, 100)
    }
    private val redDotPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.rgb(255, 60, 60)
        style = Paint.Style.FILL
    }
    private val pinkDotPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.rgb(255, 140, 180)
        style = Paint.Style.FILL
    }
    private val borderPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.rgb(180, 180, 180)
        style = Paint.Style.STROKE
        strokeWidth = 0.5f
    }

    // Black key pattern per octave (MIDI note % 12)
    private val isBlack = booleanArrayOf(
        false, true, false, true, false,  // C C# D D# E
        false, true, false, false, true,  // F F# G G# A
        false, true, false                // A# B
    )

    private val whiteKeyOffsets = intArrayOf(0, 0, 1, 1, 2, 3, 3, 4, 4, 5, 5, 6)

    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) {
        super.onSizeChanged(w, h, oldw, oldh)
        whiteKeyW = w / VISIBLE_WHITE_KEYS
        whiteKeyH = h.toFloat()
        blackKeyW = whiteKeyW * 0.6f
        blackKeyH = whiteKeyH * 0.58f
    }

    /** Total width of all white keys so HorizontalScrollView can scroll */
    fun getTotalWhiteKeys(): Int {
        val lastNote = NOTE_END
        val total = (lastNote - NOTE_START) / 12 * WHITE_KEYS_PER_OCTAVE +
                whiteKeyOffsets[(lastNote - NOTE_START) % 12] + 1
        return total
    }

    fun getTotalWidth(): Float = getTotalWhiteKeys() * whiteKeyW

    override fun onDraw(canvas: Canvas) {
        super.onDraw(canvas)
        if (whiteKeyW <= 0) return

        // --- Draw white keys ---
        for (note in NOTE_START..NOTE_END) {
            if (isBlack[note % 12]) continue
            val wkIdx = getWhiteKeyIndex(note)
            val left = wkIdx * whiteKeyW
            val rect = RectF(left, 0f, left + whiteKeyW, whiteKeyH)

            val paint = if (note in pressedNotes) whitePressedPaint else whitePaint
            canvas.drawRoundRect(rect, 3f, 3f, paint)
            canvas.drawRoundRect(rect, 3f, 3f, borderPaint)
        }

        // --- Draw black keys ---
        for (note in NOTE_START..NOTE_END) {
            if (!isBlack[note % 12]) continue
            val wkIdx = getWhiteKeyIndex(note) - 1  // black key sits on the gap
            val left = (wkIdx + 1) * whiteKeyW - blackKeyW / 2f
            val rect = RectF(left, 0f, left + blackKeyW, blackKeyH)

            val paint = if (note in pressedNotes) blackPressedPaint else blackPaint
            canvas.drawRoundRect(rect, 2f, 2f, paint)
            canvas.drawRoundRect(rect, 2f, 2f, borderPaint)
        }

        // --- Draw dots (red = pressed, pink = sustained) ---
        val dotRadius = whiteKeyW * 0.12f
        for (note in NOTE_START..NOTE_END) {
            val hasRed = note in pressedNotes
            val hasPink = note in sustainedNotes && !hasRed
            if (!hasRed && !hasPink) continue

            val dotPaint = if (hasRed) redDotPaint else pinkDotPaint
            val wkIdx = if (isBlack[note % 12]) getWhiteKeyIndex(note) - 1 else getWhiteKeyIndex(note)
            val cx = if (isBlack[note % 12]) (wkIdx + 1) * whiteKeyW
                     else wkIdx * whiteKeyW + whiteKeyW / 2f
            val cy = if (isBlack[note % 12]) blackKeyH + dotRadius * 2f
                     else whiteKeyH - dotRadius * 3f

            canvas.drawCircle(cx, cy, dotRadius, dotPaint)
        }
    }

    private fun getWhiteKeyIndex(note: Int): Int {
        val octave = (note - NOTE_START) / 12
        return octave * WHITE_KEYS_PER_OCTAVE + whiteKeyOffsets[(note - NOTE_START) % 12]
    }
}
