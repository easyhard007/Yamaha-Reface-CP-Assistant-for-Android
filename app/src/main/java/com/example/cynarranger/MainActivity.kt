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
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import java.io.File
import java.io.FileOutputStream
import android.widget.ArrayAdapter
import android.widget.Spinner
import android.widget.AdapterView
import android.view.View

class MainActivity : AppCompatActivity() {

    private lateinit var midiManager: MidiManager
    private var midiDevice: MidiDevice? = null
    private lateinit var statusText: TextView

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        statusText = findViewById(R.id.sample_text) // 确保你在 XML 里有这个 ID

        // 1. 初始化引擎 (保持不变)
        val sf2File = File(cacheDir, "FluidR3_GM.sf2")
        if (!sf2File.exists()) {
            assets.open("FluidR3_GM.sf2").use { input ->
                FileOutputStream(sf2File).use { output -> input.copyTo(output) }
            }
        }
        nativeInit(sf2File.absolutePath)
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
        // 3. MIDI 初始化
        // ==========================================
        midiManager = getSystemService(Context.MIDI_SERVICE) as MidiManager

        // 注册插拔监听
        midiManager.registerDeviceCallback(object : MidiManager.DeviceCallback() {
            override fun onDeviceAdded(deviceInfo: MidiDeviceInfo) {
                statusText.text = "发现设备: ${deviceInfo.properties.getString(MidiDeviceInfo.PROPERTY_NAME)}"
                openMidiDevice(deviceInfo)
            }

            override fun onDeviceRemoved(deviceInfo: MidiDeviceInfo) {
                statusText.text = "设备断开"
                closeMidiDevice()
            }
        }, Handler(Looper.getMainLooper()))

        // 检查当前是否已经插着设备
        val devices = midiManager.devices
        if (devices.isNotEmpty()) {
            statusText.text = "发现已连接设备: ${devices[0].properties.getString(MidiDeviceInfo.PROPERTY_NAME)}"
            openMidiDevice(devices[0])
        } else {
            statusText.text = "请插入 USB MIDI 键盘..."
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

    private fun closeMidiDevice() {
        midiDevice?.close()
        midiDevice = null
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


    external fun nativeMidiControlChange(controller: Int, value: Int)
    external fun nativeGetInstrumentCount(): Int
    external fun nativeGetInstrumentName(index: Int): String
    external fun nativeSetInstrument(index: Int)
    external fun nativeInit(sf2Path: String)
    external fun nativeNoteOn(note: Int, velocity: Int)
    external fun nativeNoteOff(note: Int)

    companion object {
        init {
            System.loadLibrary("cynarranger")
        }
    }
}