package com.chenyinan.reface_cp_assist

import android.annotation.SuppressLint
import android.content.Context
import android.media.AudioDeviceInfo
import android.media.AudioManager
import android.media.midi.MidiDevice
import android.media.midi.MidiDeviceInfo
import android.media.midi.MidiManager
import android.media.midi.MidiReceiver
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.util.Log
import android.webkit.JavascriptInterface
import android.webkit.WebChromeClient
import android.webkit.WebView
import android.webkit.WebViewClient
import androidx.appcompat.app.AppCompatActivity
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.io.FileOutputStream

class MainActivity : AppCompatActivity() {

    private lateinit var midiManager: MidiManager
    private var midiDevice: MidiDevice? = null
    private var midiOutputPort: android.media.midi.MidiOutputPort? = null
    private var midiDeviceInfoList: List<MidiDeviceInfo> = emptyList()
    private val midiUtil = MidiUtil()
    private lateinit var webView: WebView
    private var isRefaceConnected = false
    private var isConnecting = false
    private var connectedDeviceName = ""
    private val midiReceiver by lazy { MyMidiReceiver() }  // singleton
    private var jsReady = false

    private val soundFontNames = listOf("JJazzLab-SoundFont")
    private val soundFontFiles = listOf("JJazzLab-SoundFont.sf2")
    private var currentSf2Index = 0
    private val noteNames = arrayOf("C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B")

    @SuppressLint("SetJavaScriptEnabled")
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        webView = findViewById(R.id.webView)
        webView.settings.apply {
            javaScriptEnabled = true; domStorageEnabled = true
            allowFileAccess = true; allowContentAccess = true
        }
        webView.webChromeClient = object : WebChromeClient() {
            override fun onConsoleMessage(msg: android.webkit.ConsoleMessage): Boolean {
                Log.d("WebView", "[${msg.messageLevel()}] ${msg.message()}")
                return true
            }
        }
        webView.webViewClient = object : WebViewClient() {
            override fun onPageFinished(view: WebView?, url: String?) {
                jsReady = true
                pushDeviceList()
                pushSf2List()
                pushInstrumentList()
                // Sync current connection state
                if (isRefaceConnected) {
                    val idx = midiDeviceInfoList.indexOfFirst { it.properties.getString(MidiDeviceInfo.PROPERTY_NAME)?.contains("reface CP", true) == true }
                    pushDeviceState(true, connectedDeviceName, idx)
                }
                startChordPolling()
            }
        }
        webView.addJavascriptInterface(MidiJsBridge(midiUtil,
            { idx -> startDevice(idx) }, { stopDevice() },
            { idx -> selectSf2(idx) }, { idx -> nativeSetInstrument(idx) },
            { delta -> nativeChangeSplitPoint(delta) },
            { enabled -> nativeSetAutoSustain(enabled) },
            { enabled -> nativeSetBassEnhance(enabled) }
        ), "Android")
        webView.loadUrl("file:///android_asset/web/index.html")

        // Log TX messages from MidiUtil
        midiUtil.onSendLog = { text -> jsLog(text, false) }

        copyAssets()
        val defaultSf2 = File(cacheDir, soundFontFiles[0])
        nativeInit(defaultSf2.absolutePath)

        midiManager = getSystemService(Context.MIDI_SERVICE) as MidiManager
        midiManager.registerDeviceCallback(object : MidiManager.DeviceCallback() {
            override fun onDeviceAdded(info: MidiDeviceInfo) = runOnUiThread { pushDeviceList() }
            override fun onDeviceRemoved(info: MidiDeviceInfo) {
                runOnUiThread {
                    pushDeviceList()
                    if (midiDevice?.info == info) closeMidiDevice()
                }
            }
        }, Handler(Looper.getMainLooper()))

        pushDeviceList()
        val handler = Handler(Looper.getMainLooper())
        handler.postDelayed({ pushDeviceList() }, 1000)
        handler.postDelayed({ pushDeviceList() }, 3000)
    }

    // ==========================================
    // JS 通信
    // ==========================================
    private fun js(cmd: String) {
        if (!jsReady) return
        webView.post { webView.evaluateJavascript(cmd, null) }
    }

    private fun jsLog(text: String, isRx: Boolean) {
        // Escape single quotes for JS
        val escaped = text.replace("\\", "\\\\").replace("'", "\\'")
        js("if(typeof onNativeMidiLogEntry==='function')onNativeMidiLogEntry('$escaped',$isRx);")
    }

    private fun pushDeviceList() {
        midiDeviceInfoList = midiManager.devices.toList()
        // Also auto-connect Reface CP
        if (!isRefaceConnected) {
            for (info in midiDeviceInfoList) {
                val name = info.properties.getString(MidiDeviceInfo.PROPERTY_NAME) ?: ""
                if (name.contains("reface CP", ignoreCase = true)) {
                    connectDevice(info)
                    break
                }
            }
        }
        // Push to JS
        val arr = JSONArray()
        for (info in midiDeviceInfoList) {
            val obj = JSONObject()
            obj.put("name", info.properties.getString(MidiDeviceInfo.PROPERTY_NAME) ?: "Unknown")
            obj.put("manufacturer", info.properties.getString(MidiDeviceInfo.PROPERTY_MANUFACTURER) ?: "")
            arr.put(obj)
        }
        js("if(typeof onNativeMidiDeviceList==='function')onNativeMidiDeviceList('${arr.toString().replace("'", "\\'")}');")
    }

    private fun pushDeviceState(connected: Boolean, name: String, idx: Int) {
        js("if(typeof onNativeDeviceState==='function')onNativeDeviceState($connected,'$name',$idx);")
    }

    private fun handleProcessResult(result: Int) {
        val sustainCC = result and 0xFF
        if (sustainCC == 127) midiUtil.sendSustainOn()
        else if (sustainCC == 0) midiUtil.sendSustainOff()
        val bassNote = ((result shr 8) and 0xFF) - 1
        if (bassNote >= 0) {
            val bassVel = (result shr 16) and 0xFF
            if (bassVel > 0) midiUtil.sendNoteOn(bassNote, bassVel)
            else midiUtil.sendNoteOff(bassNote)
        }
    }

    private fun midiLogRx(text: String) {
        val prefix = if (connectedDeviceName.isNotEmpty()) "[$connectedDeviceName] " else ""
        jsLog("$prefix$text", true)
    }

    private val audioManager by lazy { getSystemService(Context.AUDIO_SERVICE) as AudioManager }

    private fun pushAudioDevices() {
        val devices = audioManager.getDevices(AudioManager.GET_DEVICES_OUTPUTS)
        val sb = StringBuilder()
        for (d in devices) {
            val typeName = when (d.type) {
                AudioDeviceInfo.TYPE_BUILTIN_SPEAKER -> "Speaker"
                AudioDeviceInfo.TYPE_WIRED_HEADPHONES -> "Headphones"
                AudioDeviceInfo.TYPE_WIRED_HEADSET -> "Headset"
                AudioDeviceInfo.TYPE_BLUETOOTH_A2DP -> "BT"
                AudioDeviceInfo.TYPE_USB_HEADSET -> "USB"
                else -> "?${d.type}"
            }
            val active = if (d.isSink) "*" else ""
            val name = d.productName?.toString() ?: ""
            sb.append("$active$typeName ${name}, ")
        }
        val info = if (sb.isEmpty()) "-" else sb.toString().trimEnd(',', ' ')
        js("var el=document.getElementById('dbg-audio');if(el)el.textContent='$info';")
    }

    private fun midiNoteName(note: Int) = "${noteNames[note % 12]}${note / 12 - 1}"

    // ==========================================
    // MIDI 设备连接
    // ==========================================
    private fun startDevice(index: Int) {
        if (index < 0 || index >= midiDeviceInfoList.size) return
        connectDevice(midiDeviceInfoList[index])
    }

    private fun stopDevice() {
        closeMidiDevice()
    }

    private fun connectDevice(info: MidiDeviceInfo) {
        if (isConnecting) return
        isConnecting = true
        closeMidiDevice()  // close OLD connection first
        val name = info.properties.getString(MidiDeviceInfo.PROPERTY_NAME) ?: "Unknown"
        connectedDeviceName = name
        isRefaceConnected = name.contains("reface CP", ignoreCase = true)
        midiManager.openDevice(info, { dev ->
            isConnecting = false
            if (dev == null) { isRefaceConnected = false; connectedDeviceName = ""; return@openDevice }
            midiDevice = dev
            midiUtil.connect(dev, info)
            val port = info.ports.find { it.type == MidiDeviceInfo.PortInfo.TYPE_OUTPUT }
            if (port != null) {
                val p = dev.openOutputPort(port.portNumber)
                if (p != null) {
                    midiOutputPort?.close()
                    midiOutputPort = p
                    p.connect(midiReceiver)
                    val idx = midiDeviceInfoList.indexOf(info)
                    pushDeviceState(true, name, idx)
                    midiUtil.sendTestTones()
                }
            }
        }, Handler(Looper.getMainLooper()))
    }

    private fun closeMidiDevice() {
        midiUtil.disconnect()
        midiOutputPort?.close()
        midiOutputPort = null
        midiDevice?.close()
        midiDevice = null
        isRefaceConnected = false
        connectedDeviceName = ""
        pushDeviceState(false, "", -1)
    }

    // ==========================================
    // 和弦轮询
    // ==========================================
    private val chordHandler = Handler(Looper.getMainLooper())
    private var chordRunnable: Runnable? = null

    private fun pushSf2List() {
        val arr = JSONArray()
        for (name in soundFontNames) arr.put(name)
        js("if(typeof onNativeSf2List==='function')onNativeSf2List('${arr.toString().replace("'", "\\'")}');")
    }
    private fun pushInstrumentList() {
        val count = nativeGetInstrumentCount()
        val arr = JSONArray()
        for (i in 0 until count) arr.put(nativeGetInstrumentName(i))
        js("if(typeof onNativeInstList==='function')onNativeInstList('${arr.toString().replace("'", "\\'")}');")
    }
    private fun selectSf2(index: Int) {
        if (index < 0 || index >= soundFontFiles.size) return
        currentSf2Index = index
        val file = File(cacheDir, soundFontFiles[index])
        nativeLoadSoundFont(file.absolutePath)
        // Refresh instrument list after SF2 change
        chordHandler.postDelayed({ pushInstrumentList() }, 500)
        jsLog("↑ Load SF2: ${soundFontNames[index]}", false)
    }

    private fun startChordPolling() {
        chordRunnable = object : Runnable {
            override fun run() {
                // 音符状态 → JS
                val noteState = nativeGetNoteState()
                js("if(typeof onNativeNoteState==='function')onNativeNoteState('$noteState');")
                // 和弦信息 → JS (C++ chord_detect)
                val ci = nativeGetChordInfo()
                val ciParts = ci.split("|")
                if (ciParts.size >= 5 && ciParts[0] != "--") {
                    js("if(typeof onNativeChordInfo==='function')onNativeChordInfo('${ciParts[0]}','${ciParts[1]}','${ciParts[2]}','${ciParts[3]}','${ciParts[4]}');")
                }
                // 自动踏板 CC64 发送
                val cc = nativeGetPendingSustainCC()
                if (cc == 127) midiUtil.sendSustainOn() else if (cc == 0) midiUtil.sendSustainOff()
                pushAudioDevices()
                chordHandler.postDelayed(this, 150)
            }
        }
        chordHandler.post(chordRunnable!!)
    }

    // ==========================================
    // MIDI 接收器
    // ==========================================
    inner class MyMidiReceiver : MidiReceiver() {
        override fun onSend(msg: ByteArray?, offset: Int, count: Int, timestamp: Long) {
            if (msg == null) return
            try {
                var i = offset
                while (i < offset + count) {
                    val status = msg[i].toInt() and 0xFF
                    val ch = (status and 0x0F) + 1
                    if (status in 0x90..0x9F && i + 2 < offset + count) {
                        val note = msg[i + 1].toInt()
                        val velocity = msg[i + 2].toInt()
                        if (velocity > 0) {
                            nativeNoteOn(note, velocity)
                                handleProcessResult(nativeProcessNoteOn(note, velocity))
                            val log = "↓ NoteOn  ${midiNoteName(note)}  v$velocity  ch$ch"
                            midiLogRx(log)
                            js("if(typeof onNativeMidi==='function')onNativeMidi('noteon',$note,$velocity,$ch);")
                        } else {
                            nativeNoteOff(note)
                                handleProcessResult(nativeProcessNoteOff(note))
                            val log = "↓ NoteOff ${midiNoteName(note)}  v0  ch$ch"
                            midiLogRx(log)
                            js("if(typeof onNativeMidi==='function')onNativeMidi('noteoff',$note,0,$ch);")
                        }
                        i += 3
                    } else if (status in 0x80..0x8F && i + 2 < offset + count) {
                        val note = msg[i + 1].toInt()
                        nativeNoteOff(note)
                                handleProcessResult(nativeProcessNoteOff(note))
                        val log = "↓ NoteOff ${midiNoteName(note)}  ch$ch"
                        midiLogRx(log)
                        js("if(typeof onNativeMidi==='function')onNativeMidi('noteoff',$note,0,$ch);")
                        i += 3
                    } else if (status in 0xB0..0xBF && i + 2 < offset + count) {
                        val ctrl = msg[i + 1].toInt()
                        val value = msg[i + 2].toInt()
                        nativeSendCC(ctrl, value)
                        handleProcessResult(nativeProcessCC(ctrl, value))
                        val ccName = if (ctrl == 64) "Sustain" else "CC$ctrl"
                        midiLogRx("↓ $ccName=$value  ch$ch")
                        js("if(typeof onNativeMidi==='function')onNativeMidi('cc',$ctrl,$value,$ch);")
                        i += 3
                    } else { i++ }
                }
            } catch (e: Exception) { Log.e("Midi", "parse error", e) }
        }
    }

    // ==========================================
    // Assets
    // ==========================================
    private fun copyAssets() {
        for (name in soundFontFiles) {
            val f = File(cacheDir, name)
            if (!f.exists()) {
                try { assets.open(name).use { i -> FileOutputStream(f).use { o -> i.copyTo(o) } } }
                catch (_: Exception) {}
            }
        }
    }

    override fun onDestroy() {
        super.onDestroy()
        closeMidiDevice()
    }

    // ==========================================
    // JNI
    // ==========================================
    external fun nativeInit(sf2Path: String)
    external fun nativeNoteOn(note: Int, velocity: Int)
    external fun nativeNoteOff(note: Int)
    external fun nativeProcessNoteOn(note: Int, velocity: Int): Int
    external fun nativeProcessNoteOff(note: Int): Int
    external fun nativeProcessCC(controller: Int, value: Int): Int
    external fun nativeSendCC(controller: Int, value: Int)
    external fun nativeGetChordInfo(): String
    external fun nativeGetNoteState(): String
    external fun nativeChangeSplitPoint(delta: Int): Int
    external fun nativeSetAutoSustain(enabled: Boolean)
    external fun nativeSetBassEnhance(enabled: Boolean)
    external fun nativeGetPendingSustainCC(): Int
    external fun nativeLoadSoundFont(path: String)
    external fun nativeGetInstrumentCount(): Int
    external fun nativeGetInstrumentName(index: Int): String
    external fun nativeSetInstrument(index: Int)

    companion object {
        init { System.loadLibrary("cynarranger") }
    }
}
