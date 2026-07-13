package com.chenyinan.reface_cp_assist

import android.media.midi.MidiDevice
import android.media.midi.MidiDeviceInfo
import android.media.midi.MidiInputPort
import android.util.Log
import java.io.IOException

/**
 * MIDI 工具类：向外部 MIDI 输出设备发送消息
 * 参考 Yamaha Reface CP Assistant 的 midi_util.js
 */
class MidiUtil {

    companion object {
        private const val TAG = "MidiUtil"
        const val NOTE_ON = 0x90
        const val NOTE_OFF = 0x80
        const val CC = 0xB0
        const val CC_SUSTAIN = 64
    }

    var onSendLog: ((String) -> Unit)? = null

    private var midiDevice: MidiDevice? = null
    private var midiInputPort: MidiInputPort? = null

    private val noteNames = arrayOf("C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B")
    private fun midiNoteName(note: Int): String {
        val octave = note / 12 - 1
        return "${noteNames[note % 12]}$octave"
    }

    /**
     * 连接到目标的 INPUT 端口（从 app 向设备发送数据）
     * @param device 已打开的 MidiDevice
     * @param deviceInfo 用于查找端口号
     */
    fun connect(device: MidiDevice, deviceInfo: MidiDeviceInfo) {
        disconnect()
        midiDevice = device
        val portInfo = deviceInfo.ports.find { it.type == MidiDeviceInfo.PortInfo.TYPE_INPUT }
        if (portInfo != null) {
            midiInputPort = device.openInputPort(portInfo.portNumber)
            if (midiInputPort == null) {
                Log.e(TAG, "无法打开 MIDI 输入端口")
            }
        } else {
            Log.w(TAG, "目标设备无 INPUT 端口，无法发送 MIDI")
        }
    }

    fun disconnect() {
        try { midiInputPort?.close() } catch (_: IOException) {}
        midiInputPort = null
        midiDevice = null
    }

    /** 发送 Note On (默认 Channel 1) */
    fun sendNoteOn(note: Int, velocity: Int) {
        sendMidiMessage(NOTE_ON, note, velocity)
    }

    /** 发送 Note Off (默认 Channel 1) */
    fun sendNoteOff(note: Int) {
        sendMidiMessage(NOTE_OFF, note, 0)
    }

    /** 发送 Control Change (默认 Channel 1) */
    fun sendCC(controller: Int, value: Int) {
        sendMidiMessage(CC, controller, value)
    }

    /** 发送延音踏板 On */
    fun sendSustainOn() {
        sendCC(CC_SUSTAIN, 127)
    }

    /** 发送延音踏板 Off */
    fun sendSustainOff() {
        sendCC(CC_SUSTAIN, 0)
    }

    /**
     * 发送测试音 E5(76), G5(79), C6(84)
     * 每个音间隔 150ms，每个音持续 600ms
     */
    fun sendTestTones() {
        val notes = intArrayOf(76, 79, 84) // E5, G5, C6
        val handler = android.os.Handler(android.os.Looper.getMainLooper())

        notes.forEachIndexed { index, note ->
            handler.postDelayed({
                sendNoteOn(note, 80)
                handler.postDelayed({
                    sendNoteOff(note)
                }, 600)
            }, (index * 150).toLong())
        }
    }

    private fun sendMidiMessage(status: Int, data1: Int, data2: Int) {
        val port = midiInputPort ?: return
        try {
            val msg = byteArrayOf(status.toByte(), data1.toByte(), data2.toByte())
            port.send(msg, 0, msg.size)
            // Log what we sent
            val ch = (status and 0x0F) + 1
            when (status and 0xF0) {
                0x90 -> onSendLog?.invoke("↑ NoteOn  ${midiNoteName(data1)}  v$data2  ch$ch")
                0x80 -> onSendLog?.invoke("↑ NoteOff ${midiNoteName(data1)}  ch$ch")
                0xB0 -> {
                    val ccName = if (data1 == 64) "Sustain" else "CC${data1}"
                    onSendLog?.invoke("↑ $ccName=${data2}  ch$ch")
                }
            }
        } catch (e: Exception) {
            Log.e(TAG, "发送 MIDI 失败: ${e.message}")
        }
    }
}

