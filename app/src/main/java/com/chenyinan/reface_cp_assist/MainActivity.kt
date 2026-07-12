package com.chenyinan.reface_cp_assist

import android.content.Context
import android.media.midi.MidiDevice
import android.media.midi.MidiDeviceInfo
import android.media.midi.MidiManager
import android.media.midi.MidiReceiver
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.text.SpannableStringBuilder
import android.text.Spanned
import android.text.style.ForegroundColorSpan
import android.util.Log
import android.view.View
import android.widget.Button
import android.widget.ScrollView
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import java.io.File
import java.io.FileOutputStream
import android.widget.ArrayAdapter
import android.widget.Spinner
import android.widget.AdapterView
import android.widget.SeekBar
import android.widget.LinearLayout

class MainActivity : AppCompatActivity() {

    private lateinit var midiManager: MidiManager
    private var midiDevice: MidiDevice? = null
    private var midiDeviceInfoList: List<MidiDeviceInfo> = emptyList()
    private val midiUtil = MidiUtil()
    private lateinit var statusText: TextView
    private lateinit var tvMidiLog: TextView
    private val midiLogLines = ArrayList<String>()
    private val MAX_LOG_LINES = 200
    private var isRefaceConnected = false
    private var pressedNotes = mutableSetOf<Int>()
    private var sustainedNotes = mutableSetOf<Int>()
    private lateinit var pianoView: PianoView

    // UI containers
    private lateinit var mainContent: LinearLayout
    private lateinit var settingsContent: ScrollView
    private lateinit var fabSettings: Button

    private val soundFontFiles = listOf("JJazzLab-SoundFont.sf2")
    private val soundFontNames = listOf("JJazzLab-SoundFont")

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        statusText = findViewById(R.id.sample_text)
        tvMidiLog = findViewById(R.id.tvMidiLog)
        mainContent = findViewById(R.id.mainContent)
        settingsContent = findViewById(R.id.settingsContent)
        fabSettings = findViewById(R.id.fabSettings)
        pianoView = findViewById(R.id.pianoView)

        midiUtil.onSendLog = { text -> midiLog(text) }

        // 1. 拷贝 SF2 文件
        copyAssets()

        // 2. 初始化引擎
        val defaultSf2 = File(cacheDir, soundFontFiles[0])
        nativeInit(defaultSf2.absolutePath)

        // 3. 设置界面控件
        setupSpinners()
        setupVolumeControl()
        setupInstrumentSpinner()

        // 4. MIDI
        midiManager = getSystemService(Context.MIDI_SERVICE) as MidiManager
        setupMidiSpinner()

        midiManager.registerDeviceCallback(object : MidiManager.DeviceCallback() {
            override fun onDeviceAdded(deviceInfo: MidiDeviceInfo) {
                runOnUiThread { refreshMidiDevices() }
            }
            override fun onDeviceRemoved(deviceInfo: MidiDeviceInfo) {
                runOnUiThread {
                    refreshMidiDevices()
                    if (midiDevice != null && midiDevice?.info == deviceInfo) {
                        closeMidiDevice()
                        statusText.text = "设备已断开"
                        updateFabState()
                    }
                }
            }
        }, Handler(Looper.getMainLooper()))

        refreshMidiDevices()

        // 延迟重扫：某些设备不会立即出现在 device list 中
        val handler = Handler(Looper.getMainLooper())
        handler.postDelayed({ refreshMidiDevices() }, 1000)
        handler.postDelayed({ refreshMidiDevices() }, 3000)

        // 5. 和弦轮询
        startChordPolling()

        // 6. FAB + 返回按钮
        setupFabAndBack()
    }

    // ==========================================
    // 界面切换
    // ==========================================
    private fun setupFabAndBack() {
        fabSettings.setOnClickListener { showSettings() }
        findViewById<Button>(R.id.btnBack).setOnClickListener { showMain() }
    }

    private fun showSettings() {
        mainContent.visibility = View.INVISIBLE
        settingsContent.visibility = View.VISIBLE
        fabSettings.visibility = View.INVISIBLE
        refreshMidiDevices() // 每次进入设置都刷新设备列表
    }

    private fun showMain() {
        settingsContent.visibility = View.INVISIBLE
        mainContent.visibility = View.VISIBLE
        fabSettings.visibility = View.VISIBLE
    }

    private fun updateFabState() {
        if (isRefaceConnected) {
            fabSettings.text = "✓"
            fabSettings.setTextColor(0xFF00E676.toInt())
        } else {
            fabSettings.text = "?"
            fabSettings.setTextColor(0xFFFFFFFF.toInt())
        }
    }

    // ==========================================
    // 和弦轮询
    // ==========================================
    private val chordHandler = Handler(Looper.getMainLooper())
    private var chordPollRunnable: Runnable? = null

    private fun startChordPolling() {
        val tvChord = findViewById<TextView>(R.id.tvChordInfo)
        chordPollRunnable = object : Runnable {
            override fun run() {
                val info = nativeGetChordInfo()
                val parts = info.split("|")
                if (parts.size == 3) {
                    tvChord.text = "${parts[0]}  |  ${parts[1]}  |  ${parts[2]}"
                }
                chordHandler.postDelayed(this, 200)
            }
        }
        chordHandler.post(chordPollRunnable!!)
    }

    // ==========================================
    // MIDI 日志
    // ==========================================
    private val noteNames = arrayOf("C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B")
    private val colorRx = 0xFF00E676.toInt()
    private val colorTx = 0xFFFF9800.toInt()

    private fun midiNoteName(note: Int): String {
        return "${noteNames[note % 12]}${note / 12 - 1}"
    }

    private fun midiLog(text: String) {
        runOnUiThread {
            midiLogLines.add(text)
            while (midiLogLines.size > MAX_LOG_LINES) midiLogLines.removeAt(0)
            val sb = SpannableStringBuilder()
            for ((i, line) in midiLogLines.withIndex()) {
                if (i > 0) sb.append("\n")
                val color = if (line.startsWith("↓")) colorRx else colorTx
                sb.append(line, ForegroundColorSpan(color), Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
            }
            tvMidiLog.text = sb
            val scroll = findViewById<ScrollView>(R.id.midiLogScroll)
            scroll.post { scroll.fullScroll(View.FOCUS_DOWN) }
        }
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

                    if (status in 0x90..0x9F) {
                        if (i + 2 < offset + count) {
                            val note = msg[i + 1].toInt()
                            val velocity = msg[i + 2].toInt()
                            if (velocity > 0) {
                                midiLog("↓ NoteOn  ${midiNoteName(note)}  v$velocity  ch$ch")
                                nativeNoteOn(note, velocity)
                                pressedNotes.add(note)
                                sustainedNotes.remove(note)
                            } else {
                                midiLog("↓ NoteOff ${midiNoteName(note)}  v0  ch$ch")
                                nativeNoteOff(note)
                                pressedNotes.remove(note)
                            }
                            pianoView.pressedNotes = pressedNotes.toSet()
                            pianoView.sustainedNotes = sustainedNotes.toSet()
                            i += 3
                        } else { break }
                    } else if (status in 0x80..0x8F) {
                        if (i + 2 < offset + count) {
                            val note = msg[i + 1].toInt()
                            midiLog("↓ NoteOff ${midiNoteName(note)}  ch$ch")
                            nativeNoteOff(note)
                            pressedNotes.remove(note)
                            pianoView.pressedNotes = pressedNotes.toSet()
                            pianoView.sustainedNotes = sustainedNotes.toSet()
                            i += 3
                        } else { break }
                    } else if (status in 0xB0..0xBF) {
                        if (i + 2 < offset + count) {
                            val controller = msg[i + 1].toInt()
                            val value = msg[i + 2].toInt()
                            val ccName = if (controller == 64) "Sustain" else "CC$controller"
                            midiLog("↓ $ccName=$value  ch$ch")
                            nativeMidiControlChange(controller, value)
                            i += 3
                        } else { i++ }
                    } else {
                        i++
                    }
                }
            } catch (e: Exception) {
                Log.e("Midi", "解析错误: ${e.message}")
            }
        }
    }

    // ==========================================
    // 文件拷贝
    // ==========================================
    private fun copyAssets() {
        for (fileName in soundFontFiles) {
            val file = File(cacheDir, fileName)
            if (!file.exists()) {
                try {
                    assets.open(fileName).use { input ->
                        FileOutputStream(file).use { output -> input.copyTo(output) }
                    }
                } catch (e: Exception) { e.printStackTrace() }
            }
        }
    }

    // ==========================================
    // Spinners
    // ==========================================
    private fun setupSpinners() {
        val sfSpinner = findViewById<Spinner>(R.id.sfSpinner)
        val sfAdapter = ArrayAdapter(this, R.layout.spinner_item, soundFontNames)
        sfAdapter.setDropDownViewResource(R.layout.spinner_dropdown)
        sfSpinner.adapter = sfAdapter
        sfSpinner.onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
            override fun onItemSelected(parent: AdapterView<*>, view: View?, position: Int, id: Long) {
                val file = File(cacheDir, soundFontFiles[position])
                nativeLoadSoundFont(file.absolutePath)
                refreshInstrumentSpinner()
            }
            override fun onNothingSelected(parent: AdapterView<*>) {}
        }
    }

    private fun refreshInstrumentSpinner() {
        val instSpinner = findViewById<Spinner>(R.id.instrumentSpinner)
        val count = nativeGetInstrumentCount()
        val names = ArrayList<String>()
        for (i in 0 until count) names.add(nativeGetInstrumentName(i))
        val adapter = ArrayAdapter(this, R.layout.spinner_item, names)
        adapter.setDropDownViewResource(R.layout.spinner_dropdown)
        instSpinner.adapter = adapter
        instSpinner.onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
            override fun onItemSelected(parent: AdapterView<*>, view: View?, position: Int, id: Long) {
                nativeSetInstrument(position)
            }
            override fun onNothingSelected(parent: AdapterView<*>) {}
        }
    }

    private fun setupInstrumentSpinner() {
        val spinner = findViewById<Spinner>(R.id.instrumentSpinner)
        val count = nativeGetInstrumentCount()
        val names = ArrayList<String>()
        for (i in 0 until count) names.add(nativeGetInstrumentName(i))
        val adapter = ArrayAdapter(this, R.layout.spinner_item, names)
        adapter.setDropDownViewResource(R.layout.spinner_dropdown)
        spinner.adapter = adapter
        spinner.onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
            override fun onItemSelected(parent: AdapterView<*>, view: View?, position: Int, id: Long) {
                nativeSetInstrument(position)
            }
            override fun onNothingSelected(parent: AdapterView<*>) {}
        }
    }

    // ==========================================
    // 音量
    // ==========================================
    private fun setupVolumeControl() {
        val seekBar = findViewById<SeekBar>(R.id.volumeSeekBar)
        val label = findViewById<TextView>(R.id.tvVolumeLabel)
        seekBar.progress = 20
        seekBar.setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
            override fun onProgressChanged(seekBar: SeekBar?, progress: Int, fromUser: Boolean) {
                val gain = progress * 0.05f
                label.text = String.format("Master Volume: %.2f", gain)
                nativeSetMasterVolume(gain)
            }
            override fun onStartTrackingTouch(seekBar: SeekBar?) {}
            override fun onStopTrackingTouch(seekBar: SeekBar?) {}
        })
    }

    // ==========================================
    // MIDI 设备
    // ==========================================
    private fun openMidiDevice(deviceInfo: MidiDeviceInfo) {
        midiManager.openDevice(deviceInfo, { device ->
            autoConnectingInfo = null
            if (device == null) { Log.e("Midi", "无法打开设备"); return@openDevice }
            midiDevice = device
            midiUtil.connect(device, deviceInfo)

            val portInfo = deviceInfo.ports.find { it.type == MidiDeviceInfo.PortInfo.TYPE_OUTPUT }
            if (portInfo != null) {
                val outputPort = device.openOutputPort(portInfo.portNumber)
                if (outputPort != null) {
                    outputPort.connect(MyMidiReceiver())
                    val name = deviceInfo.properties.getString(MidiDeviceInfo.PROPERTY_NAME) ?: "Unknown"
                    runOnUiThread {
                        statusText.text = "已连接: $name"
                    }
                    // Check Reface CP
                    isRefaceConnected = name.contains("reface CP", ignoreCase = true)
                    runOnUiThread { updateFabState() }
                    // 测试音
                    midiUtil.sendTestTones()
                } else { Log.e("Midi", "无法打开输出端口") }
            }
        }, Handler(Looper.getMainLooper()))
    }

    private var autoConnectingInfo: MidiDeviceInfo? = null // 正在自动连接的目标

    private fun setupMidiSpinner() {
        val midiSpinner = findViewById<Spinner>(R.id.midiSpinner)
        findViewById<Button>(R.id.btnRefreshMidi).setOnClickListener { refreshMidiDevices() }
        midiSpinner.onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
            override fun onItemSelected(parent: AdapterView<*>, view: View?, position: Int, id: Long) {
                if (position >= 0 && position < midiDeviceInfoList.size) {
                    val selected = midiDeviceInfoList[position]
                    // 自动连接进行中 → 忽略；已连接同一设备 → 忽略
                    if (selected == autoConnectingInfo) return
                    if (midiDevice?.info == selected) return
                    closeMidiDevice()
                    openMidiDevice(selected)
                }
            }
            override fun onNothingSelected(parent: AdapterView<*>) {}
        }
    }

    private fun refreshMidiDevices() {
        midiDeviceInfoList = midiManager.devices.toList()
        val midiSpinner = findViewById<Spinner>(R.id.midiSpinner)

        if (midiDeviceInfoList.isEmpty()) {
            midiSpinner.adapter = ArrayAdapter(this, R.layout.spinner_item,
                listOf("(没有找到 MIDI 设备)"))
            return
        }

        val names = midiDeviceInfoList.map { info ->
            val props = info.properties
            val name = props.getString(MidiDeviceInfo.PROPERTY_NAME) ?: "Unknown"
            val manufacturer = props.getString(MidiDeviceInfo.PROPERTY_MANUFACTURER)
            if (manufacturer != null) "$name ($manufacturer)" else name
        }

        val adapter = ArrayAdapter(this, R.layout.spinner_item, names)
        adapter.setDropDownViewResource(R.layout.spinner_dropdown)
        midiSpinner.adapter = adapter

        // 同步 Spinner 到当前已连接设备，防止 adapter 变更时跳到 position 0
        if (midiDevice != null) {
            for (i in midiDeviceInfoList.indices) {
                if (midiDeviceInfoList[i] == midiDevice?.info) {
                    autoConnectingInfo = midiDeviceInfoList[i]
                    midiSpinner.setSelection(i)
                    break
                }
            }
        }

        // Auto-select Reface CP if not already connected
        if (!isRefaceConnected) {
            for (i in midiDeviceInfoList.indices) {
                val name = midiDeviceInfoList[i].properties
                    .getString(MidiDeviceInfo.PROPERTY_NAME) ?: ""
                if (name.contains("reface CP", ignoreCase = true)) {
                    autoConnectingInfo = midiDeviceInfoList[i]
                    midiSpinner.setSelection(i)
                    closeMidiDevice()
                    openMidiDevice(midiDeviceInfoList[i])
                    midiLog("↑ Auto-select: $name")
                    break
                }
            }
        }
    }

    private fun closeMidiDevice() {
        midiUtil.disconnect()
        midiDevice?.close()
        midiDevice = null
        isRefaceConnected = false
        updateFabState()
    }

    override fun onDestroy() {
        super.onDestroy()
        closeMidiDevice()
    }

    // ==========================================
    // JNI
    // ==========================================
    external fun nativeSetMasterVolume(gain: Float)
    external fun nativeMidiControlChange(controller: Int, value: Int)
    external fun nativeGetInstrumentCount(): Int
    external fun nativeGetInstrumentName(index: Int): String
    external fun nativeSetInstrument(index: Int)
    external fun nativeInit(sf2Path: String)
    external fun nativeNoteOn(note: Int, velocity: Int)
    external fun nativeNoteOff(note: Int)
    external fun nativeGetChordInfo(): String
    external fun nativeLoadSoundFont(path: String)

    companion object {
        init { System.loadLibrary("cynarranger") }
    }
}
