package com.example.cynarranger

import android.content.Context
import android.media.midi.MidiDevice
import android.media.midi.MidiDeviceInfo
import android.media.midi.MidiManager
import android.media.midi.MidiReceiver
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.util.Log
import android.view.MotionEvent
import android.widget.Button
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import java.io.File
import java.io.FileOutputStream
import android.widget.ArrayAdapter
import android.widget.Spinner
import android.widget.AdapterView
import android.view.View
import android.widget.SeekBar

class MainActivity : AppCompatActivity() {

    private lateinit var midiManager: MidiManager
    private var midiDevice: MidiDevice? = null
    private var midiDeviceInfoList: List<MidiDeviceInfo> = emptyList()
    private lateinit var statusText: TextView

    // 定义你的音色库文件名列表
    private val soundFontFiles = listOf(
        "JJazzLab-SoundFont.sf2"
    )

    // 为了显示好看的名字，可以搞个映射，或者直接用文件名
    private val soundFontNames = listOf(
        "JJazzLab-SoundFont"
    )

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        statusText = findViewById(R.id.sample_text) // 确保你在 XML 里有这个 ID

        // 1. 拷贝所有 SF2 文件到缓存
        copyAssets()

        // 2. 初始化引擎 (默认加载第一个)
        val defaultSf2 = File(cacheDir, soundFontFiles[0])
        nativeInit(defaultSf2.absolutePath)

        // 3. 设置两个下拉菜单
        setupSpinners()

        // >>>>> 设置音量控制 >>>>>
        setupVolumeControl()
        // 加载乐器列表到 UI
        setupInstrumentSpinner()



        // 2. 按钮测试 (保持不变)
        val btnTest = findViewById<Button>(R.id.btnTest)
        btnTest.setOnTouchListener { _, event ->
            when (event.action) {
                MotionEvent.ACTION_DOWN -> nativeNoteOn(60, 100)
                MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> nativeNoteOff(60)
            }
            true
        }

        // ==========================================
        // 3. MIDI 初始化 — 手动选择设备
        // ==========================================
        midiManager = getSystemService(Context.MIDI_SERVICE) as MidiManager

        setupMidiSpinner()

        // 监听设备插拔，自动刷新列表
        midiManager.registerDeviceCallback(object : MidiManager.DeviceCallback() {
            override fun onDeviceAdded(deviceInfo: MidiDeviceInfo) {
                runOnUiThread { refreshMidiDevices() }
            }
            override fun onDeviceRemoved(deviceInfo: MidiDeviceInfo) {
                runOnUiThread {
                    refreshMidiDevices()
                    // 如果当前连接的设备被拔出，断开连接
                    if (midiDevice != null && midiDevice?.info == deviceInfo) {
                        closeMidiDevice()
                        statusText.text = "设备已断开"
                    }
                }
            }
        }, Handler(Looper.getMainLooper()))

        // 初次扫描
        refreshMidiDevices()
    }



    private fun copyAssets() {
        for (fileName in soundFontFiles) {
            val file = File(cacheDir, fileName)
            if (!file.exists()) {
                try {
                    assets.open(fileName).use { input ->
                        FileOutputStream(file).use { output ->
                            input.copyTo(output)
                        }
                    }
                } catch (e: Exception) {
                    e.printStackTrace()
                }
            }
        }
    }

    private fun setupSpinners() {
        // --- A. 设置音色库 (SoundFont) Spinner ---
        val sfSpinner = findViewById<Spinner>(R.id.sfSpinner)
        val sfAdapter = ArrayAdapter(this, android.R.layout.simple_spinner_item, soundFontNames)
        sfAdapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item)
        sfSpinner.adapter = sfAdapter

        sfSpinner.onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
            override fun onItemSelected(parent: AdapterView<*>, view: View?, position: Int, id: Long) {
                // 1. 获取选中的文件名
                val fileName = soundFontFiles[position]
                val file = File(cacheDir, fileName)

                // 2. 调用 JNI 切换引擎里的 SoundFont
                nativeLoadSoundFont(file.absolutePath)

                // 3. 核心：切换完库后，必须刷新乐器列表！
                refreshInstrumentSpinner()
            }
            override fun onNothingSelected(parent: AdapterView<*>) {}
        }
    }

    private fun refreshInstrumentSpinner() {
        val instSpinner = findViewById<Spinner>(R.id.instrumentSpinner)

        // 1. 从 C++ 获取当前库的乐器总数
        val count = nativeGetInstrumentCount()
        val instrumentNames = ArrayList<String>()

        // 2. 获取所有乐器名
        for (i in 0 until count) {
            instrumentNames.add(nativeGetInstrumentName(i))
        }

        // 3. 创建适配器并设置
        val instAdapter = ArrayAdapter(this, android.R.layout.simple_spinner_item, instrumentNames)
        instAdapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item)
        instSpinner.adapter = instAdapter

        // 4. 重置选择监听
        instSpinner.onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
            override fun onItemSelected(parent: AdapterView<*>, view: View?, position: Int, id: Long) {
                nativeSetInstrument(position)
            }
            override fun onNothingSelected(parent: AdapterView<*>) {}
        }
    }

    private fun setupInstrumentSpinner() {
        val spinner = findViewById<Spinner>(R.id.instrumentSpinner)

        // 1. 从 C++ 获取总数
        val count = nativeGetInstrumentCount()
        val instrumentNames = ArrayList<String>()

        // 2. 循环获取名字
        for (i in 0 until count) {
            instrumentNames.add(nativeGetInstrumentName(i))
        }

        // 3. 创建适配器 (Adapter) 填充数据
        val adapter = ArrayAdapter(
            this,
            android.R.layout.simple_spinner_item,
            instrumentNames
        )
        adapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item)
        spinner.adapter = adapter

        // 4. 设置选择监听器
        spinner.onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
            override fun onItemSelected(parent: AdapterView<*>, view: View?, position: Int, id: Long) {
                // 这里的 position 就是乐器在 SF2 里的 index
                nativeSetInstrument(position)

                // 如果是 USB 键盘连接状态，这一步之后你的键盘发出的声音就会变了
            }

            override fun onNothingSelected(parent: AdapterView<*>) {}
        }
    }

    private fun openMidiDevice(deviceInfo: MidiDeviceInfo) {
        midiManager.openDevice(deviceInfo, { device ->
            if (device == null) {
                Log.e("Midi", "无法打开设备")
                return@openDevice
            }
            midiDevice = device

            // 查找设备的“源”端口 (Source Port) -> 即键盘发送数据的端口
            // 有些设备有多个端口，我们通常找第一个 TYPE_OUTPUT
            val portInfo = deviceInfo.ports.find { it.type == MidiDeviceInfo.PortInfo.TYPE_OUTPUT }

            if (portInfo != null) {
                val outputPort = device.openOutputPort(portInfo.portNumber)
                if (outputPort != null) {
                    // 连接我们的接收器
                    outputPort.connect(MyMidiReceiver())
                    runOnUiThread {
                        statusText.text = "已连接: ${deviceInfo.properties.getString(MidiDeviceInfo.PROPERTY_NAME)}\n你可以演奏了!"
                    }
                } else {
                    Log.e("Midi", "无法打开输出端口")
                }
            }
        }, Handler(Looper.getMainLooper()))
    }

    private fun setupVolumeControl() {
        val seekBar = findViewById<SeekBar>(R.id.volumeSeekBar)
        val label = findViewById<TextView>(R.id.tvVolumeLabel)

        // 默认 Gain = 1.0，对应 Progress = 20 (因为 20 * 0.05 = 1.0)
        seekBar.progress = 20

        seekBar.setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
            override fun onProgressChanged(seekBar: SeekBar?, progress: Int, fromUser: Boolean) {
                // 映射算法：每1格代表 0.05 的增益
                // 0 -> 0.0
                // 20 -> 1.0 (默认)
                // 100 -> 5.0 (极大)
                val gain = progress * 0.05f

                label.text = String.format("Master Volume: %.2f", gain)

                // 调用 JNI
                nativeSetMasterVolume(gain)
            }

            override fun onStartTrackingTouch(seekBar: SeekBar?) {}
            override fun onStopTrackingTouch(seekBar: SeekBar?) {}
        })
    }

    // ==========================================
    // MIDI 设备列表
    // ==========================================
    private fun setupMidiSpinner() {
        val midiSpinner = findViewById<Spinner>(R.id.midiSpinner)
        val btnRefresh = findViewById<Button>(R.id.btnRefreshMidi)

        btnRefresh.setOnClickListener { refreshMidiDevices() }

        midiSpinner.onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
            override fun onItemSelected(parent: AdapterView<*>, view: View?, position: Int, id: Long) {
                if (position >= 0 && position < midiDeviceInfoList.size) {
                    // 切设备前先断开旧连接
                    closeMidiDevice()
                    openMidiDevice(midiDeviceInfoList[position])
                }
            }
            override fun onNothingSelected(parent: AdapterView<*>) {}
        }
    }

    private fun refreshMidiDevices() {
        midiDeviceInfoList = midiManager.devices.toList()
        val midiSpinner = findViewById<Spinner>(R.id.midiSpinner)

        if (midiDeviceInfoList.isEmpty()) {
            val emptyList = listOf("(没有找到 MIDI 设备)")
            midiSpinner.adapter = ArrayAdapter(this, android.R.layout.simple_spinner_item, emptyList)
            return
        }

        val names = midiDeviceInfoList.map { info ->
            val props = info.properties
            val name = props.getString(MidiDeviceInfo.PROPERTY_NAME) ?: "Unknown"
            val manufacturer = props.getString(MidiDeviceInfo.PROPERTY_MANUFACTURER)
            if (manufacturer != null) "$name ($manufacturer)" else name
        }

        val adapter = ArrayAdapter(this, android.R.layout.simple_spinner_item, names)
        adapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item)
        midiSpinner.adapter = adapter
    }

    private fun closeMidiDevice() {
        midiDevice?.close()
        midiDevice = null
    }

    override fun onDestroy() {
        super.onDestroy()
        closeMidiDevice()
    }

    // ==========================================
    // 4. MIDI 接收器 (核心解析逻辑)
    // ==========================================
    inner class MyMidiReceiver : MidiReceiver() {
        override fun onSend(msg: ByteArray?, offset: Int, count: Int, timestamp: Long) {
            if (msg == null) return

            // 加上 try-catch 防止解析出错导致整个 APP 挂掉
            try {
                var i = offset
                while (i < offset + count) {
                    val status = msg[i].toInt() and 0xFF

                    if (status in 0x90..0x9F) { // 处理 Note On ...
                        // 严格检查是否有足够的字节
                        if (i + 2 < offset + count) {
                            val note = msg[i + 1].toInt()
                            val velocity = msg[i + 2].toInt()
                            if (velocity > 0) nativeNoteOn(note, velocity)
                            else nativeNoteOff(note)
                            i += 3
                        } else {
                            // 数据包在中间断开了（这种情况极少见但在高速演奏时可能发生）
                            // 理想做法是缓存下来等下一个包，但在简单 Demo 里直接 break 即可
                            break
                        }
                    } else if (status in 0x80..0x8F) { // 处理 Note Off ...
                        if (i + 2 < offset + count) {
                            val note = msg[i + 1].toInt()
                            nativeNoteOff(note)
                            i += 3
                        } else { break }
                    }
                    else if (status in 0xB0..0xBF) { // 处理 Control Change (0xB0 - 0xBF)
                        if (i + 2 < offset + count) {
                            val controller = msg[i + 1].toInt()
                            val value = msg[i + 2].toInt()

                            // 将 CC 消息传给 C++
                            nativeMidiControlChange(controller, value)

                            i += 3
                        } else {
                            i++
                        }
                    }
                    else {
                        // 遇到不认识的字节（如 Pitch Bend, Active Sensing, Clock 等）必须跳过，否则 i 永远不增加，导致死循环
                        i++
                    }
                }
            } catch (e: Exception) {
                Log.e("Midi", "解析错误: ${e.message}")
            }
        }
    }

    external fun nativeSetMasterVolume(gain: Float)
    external fun nativeMidiControlChange(controller: Int, value: Int)
    external fun nativeGetInstrumentCount(): Int
    external fun nativeGetInstrumentName(index: Int): String
    external fun nativeSetInstrument(index: Int)
    external fun nativeInit(sf2Path: String)
    external fun nativeNoteOn(note: Int, velocity: Int)
    external fun nativeNoteOff(note: Int)
    external fun nativeLoadSoundFont(path: String)

    companion object {
        init {
            System.loadLibrary("cynarranger")
        }
    }
}