package com.chenyinan.reface_cp_assist

import android.util.Log
import android.webkit.JavascriptInterface

/**
 * JS Bridge: WebView 中的 JavaScript 通过此类调用 Android 原生功能
 */
class MidiJsBridge(
    private val midiUtil: MidiUtil,
    private val onStartDevice: (Int) -> Unit,
    private val onStopDevice: () -> Unit,
    private val onSelectSf2: (Int) -> Unit,
    private val onSelectInstrument: (Int) -> Unit,
    private val onChangeSplitPoint: (Int) -> Int,
    private val onToggleAutoSustain: (Boolean) -> Unit,
    private val onToggleBassEnhance: (Boolean) -> Unit,
    private val onChangeTranspose: (Int) -> Int
) {
    companion object { private const val TAG = "MidiJsBridge" }

    @JavascriptInterface
    fun sendMidiNote(note: Int, velocity: Int, isOn: Boolean) {
        if (isOn) midiUtil.sendNoteOn(note, velocity)
        else midiUtil.sendNoteOff(note)
    }

    @JavascriptInterface
    fun sendMidiCC(controller: Int, value: Int) { midiUtil.sendCC(controller, value) }

    @JavascriptInterface
    fun sendTestTones() { midiUtil.sendTestTones() }

    @JavascriptInterface
    fun startDevice(index: Int) { onStartDevice(index) }

    @JavascriptInterface
    fun stopDevice() { onStopDevice() }

    @JavascriptInterface
    fun selectSf2(index: Int) { onSelectSf2(index) }

    @JavascriptInterface
    fun selectInstrument(index: Int) { onSelectInstrument(index) }

    @JavascriptInterface
    fun changeSplitPoint(delta: Int): Int { return onChangeSplitPoint(delta) }

    @JavascriptInterface
    fun changeTranspose(delta: Int): Int { return onChangeTranspose(delta) }

    @JavascriptInterface
    fun toggleBassEnhance(enabled: Boolean) { onToggleBassEnhance(enabled) }

    @JavascriptInterface
    fun toggleAutoSustain(enabled: Boolean) { onToggleAutoSustain(enabled) }

    @JavascriptInterface
    fun log(msg: String) { Log.d(TAG, "JS: $msg") }
}
