package com.chenyinan.reface_cp_assist

import android.util.Log
import android.webkit.JavascriptInterface

class MidiJsBridge(
    private val midiUtil: MidiUtil,
    private val onStartDevice: (Int) -> Unit,
    private val onStopDevice: () -> Unit,
    private val onSelectSf2: (Int) -> Unit,
    private val onSelectInstrument: (Int) -> Unit,
    private val onGetInstrumentBank: (Int) -> Int,
    private val onChangeSplitPoint: (Int) -> Int,
    private val onToggleAutoSustain: (Boolean) -> Unit,
    private val onToggleBassEnhance: (Boolean) -> Unit,
    private val onChangeTranspose: (Int) -> Int,
    // Style
    private val onGetStyleFileList: () -> String,
    private val onLoadStyle: (Int) -> String,
    private val onSelectStyleScene: (Int) -> Unit,
    private val onStartStyle: () -> Unit,
    private val onStopStyle: () -> Unit,
    private val onIsStylePlaying: () -> Boolean,
    private val onGetCurrentStyleScene: () -> Int,
    private val onGetPendingStyleScene: () -> Int,
    private val onGetStyleTempo: () -> Double,
    private val onGetCurrentBpm: () -> Double,
    private val onGetTimeSig: () -> Int,
    private val onGetCurrentBeat: () -> Int,
    private val onSyncBeat: () -> Unit,
    private val onRequestBpmMult: (Int) -> Unit,
    private val onAdjustBpm: (Int) -> Double,
    private val onSetBpmHold: (Boolean) -> Unit,
    private val onGetChord: () -> String,
    private val onGetChordNotes: () -> String,
    private val onGetChordTiming: () -> String,
    private val onGetStyleChannels: () -> String,
    private val onSetStyleChannelInst: (Int, Int, Int) -> Unit,
    private val onDumpStyleDebug: () -> String,
    private val onGetDebugInfo: () -> String,
    private val onSetAssistType: (Int) -> Unit,
    private val onCajonTick: () -> Int,
    private val onSetCajonEnergy: (Float) -> Unit,
    private val onGetCajonEnergy: () -> Float,
    private val onSetMinCajonEnergy: (Float) -> Unit,
    private val onGetMinCajonEnergy: () -> Float,
    private val onSetRhythmGain: (Float) -> Unit,
    private val onGetRhythmGain: () -> Float,
    private val onSetBassAssist: (Boolean) -> Unit,
    private val onIsBassAssistEnabled: () -> Boolean,
    private val onSetBassAssistVolume: (Float) -> Unit,
    private val onGetBassAssistVolume: () -> Float,
    private val onGetTempoDetectorData: () -> String,
    private val onInitRhythmEngine: (String) -> Unit,
    private val onSetAccompVolume: (Double) -> Unit,
    private val onSetLeadVolume: (Double) -> Unit,
    private val onGetAccompGain: () -> Double,
    private val onGetLeadGain: () -> Double,
    private val onToggleMute: (Int) -> Unit,
    private val onGetActiveChannels: () -> Int,
    private val onIsChannelMuted: (Int) -> Boolean,
    private val onSetReverb: (Double, Double) -> Unit,
    private val onGetReverbRoomSize: () -> Double,
    private val onGetReverbLevel: () -> Double,
    private val onSetHumanize: (Float, Float) -> Unit,
    private val onGetHumanizeTiming: () -> Float,
    private val onGetHumanizeVelocity: () -> Float,
    // Drum loops
    private val onGetDrumLoopList: () -> String,
    private val onSelectDrumLoop: (String) -> Unit,
    private val onSetDrumPending: (String) -> Unit,
    private val onDrumStartPlayback: () -> Unit,
    private val onDrumStopPlayback: () -> Unit,
    private val onDrumStopImmediate: () -> Unit
) {
    companion object { private const val TAG = "MidiJsBridge" }

    @JavascriptInterface fun sendMidiNote(note: Int, velocity: Int, isOn: Boolean) {
        if (isOn) midiUtil.sendNoteOn(note, velocity) else midiUtil.sendNoteOff(note)
    }
    @JavascriptInterface fun sendMidiCC(controller: Int, value: Int) { midiUtil.sendCC(controller, value) }
    @JavascriptInterface fun sendTestTones() { midiUtil.sendTestTones() }
    @JavascriptInterface fun startDevice(index: Int) { onStartDevice(index) }
    @JavascriptInterface fun stopDevice() { onStopDevice() }
    @JavascriptInterface fun selectSf2(index: Int) { onSelectSf2(index) }
    @JavascriptInterface fun selectInstrument(index: Int) { onSelectInstrument(index) }
    @JavascriptInterface fun getInstrumentBank(index: Int): Int = onGetInstrumentBank(index)
    @JavascriptInterface fun changeSplitPoint(delta: Int): Int = onChangeSplitPoint(delta)
    @JavascriptInterface fun changeTranspose(delta: Int): Int = onChangeTranspose(delta)
    @JavascriptInterface fun toggleBassEnhance(enabled: Boolean) { onToggleBassEnhance(enabled) }
    @JavascriptInterface fun toggleAutoSustain(enabled: Boolean) { onToggleAutoSustain(enabled) }
    @JavascriptInterface fun log(msg: String) { Log.d(TAG, "JS: $msg") }

    // Style
    @JavascriptInterface fun getStyleFileList(): String = onGetStyleFileList()
    @JavascriptInterface fun loadStyleFile(fileIdx: Int): String = onLoadStyle(fileIdx)
    @JavascriptInterface fun selectStyleScene(index: Int) { onSelectStyleScene(index) }
    @JavascriptInterface fun startStyle() { onStartStyle() }
    @JavascriptInterface fun stopStyle() { onStopStyle() }
    @JavascriptInterface fun isStylePlaying(): Boolean = onIsStylePlaying()
    @JavascriptInterface fun getCurrentStyleScene(): Int = onGetCurrentStyleScene()
    @JavascriptInterface fun getPendingStyleScene(): Int = onGetPendingStyleScene()
    @JavascriptInterface fun getStyleTempo(): Double = onGetStyleTempo()
    @JavascriptInterface fun getCurrentBpm(): Double = onGetCurrentBpm()
    @JavascriptInterface fun getTimeSig(): Int = onGetTimeSig()
    @JavascriptInterface fun getCurrentBeat(): Int = onGetCurrentBeat()
    @JavascriptInterface fun syncBeat() { onSyncBeat() }
    @JavascriptInterface fun requestBpmMult(mult: Int) { onRequestBpmMult(mult) }
    @JavascriptInterface fun adjustBpm(delta: Int): Double = onAdjustBpm(delta.coerceIn(-1, 1))
    @JavascriptInterface fun setBpmHold(held: Boolean) { onSetBpmHold(held) }
    @JavascriptInterface fun getChord(): String = onGetChord()
    @JavascriptInterface fun getChordNotes(): String = onGetChordNotes()
    @JavascriptInterface fun getChordTiming(): String = onGetChordTiming()
    @JavascriptInterface fun setAccompVolume(vol: Double) { onSetAccompVolume(vol) }
    @JavascriptInterface fun setLeadVolume(vol: Double) { onSetLeadVolume(vol) }
    @JavascriptInterface fun getAccompGain(): Double = onGetAccompGain()
    @JavascriptInterface fun getLeadGain(): Double = onGetLeadGain()
    @JavascriptInterface fun getStyleChannels(): String = onGetStyleChannels()
    @JavascriptInterface fun setStyleChannelInst(channel: Int, bank: Int, program: Int) {
        onSetStyleChannelInst(channel, bank, program)
    }
    @JavascriptInterface fun dumpStyleDebug(): String = onDumpStyleDebug()
    @JavascriptInterface fun getDebugInfo(): String = onGetDebugInfo()
    @JavascriptInterface fun setAssistType(v: Int) { onSetAssistType(v) }
    @JavascriptInterface fun cajonTick(): Int = onCajonTick()
    @JavascriptInterface fun setCajonEnergy(energy: Float) { onSetCajonEnergy(energy) }
    @JavascriptInterface fun getCajonEnergy(): Float = onGetCajonEnergy()
    @JavascriptInterface fun setMinCajonEnergy(v: Float) { onSetMinCajonEnergy(v) }
    @JavascriptInterface fun getMinCajonEnergy(): Float = onGetMinCajonEnergy()
    @JavascriptInterface fun setRhythmGain(v: Float) { onSetRhythmGain(v) }
    @JavascriptInterface fun getRhythmGain(): Float = onGetRhythmGain()
    @JavascriptInterface fun setBassAssist(v: Boolean) { onSetBassAssist(v) }
    @JavascriptInterface fun isBassAssistEnabled(): Boolean = onIsBassAssistEnabled()
    @JavascriptInterface fun setBassAssistVolume(v: Float) { onSetBassAssistVolume(v) }
    @JavascriptInterface fun getBassAssistVolume(): Float = onGetBassAssistVolume()
    @JavascriptInterface fun getTempoDetectorData(): String = onGetTempoDetectorData()
    @JavascriptInterface fun initRhythmEngine(dir: String) { onInitRhythmEngine(dir) }
    @JavascriptInterface fun toggleMute(channel: Int) { onToggleMute(channel) }
    @JavascriptInterface fun getActiveChannels(): Int = onGetActiveChannels()
    @JavascriptInterface fun isChannelMuted(channel: Int): Boolean = onIsChannelMuted(channel)
    @JavascriptInterface fun setReverb(roomSize: Double, level: Double) { onSetReverb(roomSize, level) }
    @JavascriptInterface fun getReverbRoomSize(): Double = onGetReverbRoomSize()
    @JavascriptInterface fun getReverbLevel(): Double = onGetReverbLevel()
    @JavascriptInterface fun setHumanize(timing: Float, velocity: Float) { onSetHumanize(timing, velocity) }
    @JavascriptInterface fun getHumanizeTiming(): Float = onGetHumanizeTiming()
    @JavascriptInterface fun getHumanizeVelocity(): Float = onGetHumanizeVelocity()
    // Drum loops
    @JavascriptInterface fun getDrumLoopList(): String = onGetDrumLoopList()
    @JavascriptInterface fun selectDrumLoop(folder: String) { onSelectDrumLoop(folder) }
    @JavascriptInterface fun setDrumPending(variation: String) { onSetDrumPending(variation) }
    @JavascriptInterface fun drumStartPlayback() { onDrumStartPlayback() }
    @JavascriptInterface fun drumStopPlayback() { onDrumStopPlayback() }
    @JavascriptInterface fun drumStopImmediate() { onDrumStopImmediate() }
}
