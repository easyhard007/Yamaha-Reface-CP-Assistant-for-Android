package com.chenyinan.reface_cp_assist

import android.content.Context
import android.media.MediaCodec
import android.media.MediaDataSource
import android.media.MediaExtractor
import android.media.MediaFormat
import android.util.Log
import org.json.JSONArray
import org.json.JSONObject
import java.io.ByteArrayOutputStream
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicInteger

/**
 * 鼓循环管理 (双引擎, 三线程):
 *
 * 状态: 0 = 未渲染; 1 = 高质量已渲染前 0.5s; 2 = 高质量完整渲染
 *
 *  - 预热线程 (线程1): 按序号顺序用高质量 sbsms 渲染所有 variation 的前 0.5s (0→1)
 *  - 完整渲染线程 (线程2): 全部预热完成后, 按序号顺序将状态1的 variation 完整渲染 (1→2)
 *  - 按需渲染线程 (线程3):
 *      · 点击状态0的 variation → Signalsmith 低质量引擎流式渲染, 几乎立即出声
 *      · 点击状态1的 variation → 播放已渲染的 0.5s, 同时续渲染完整 (高质量)
 *      · 状态2 → 直接播放内存中的完整音频
 *
 *  - 播放状态0的 variation 时, 后台高质量渲染完成后在循环边界切换为高质量音频
 *  - BPM 变化: 全部状态重置为 0 并重新渲染, 正在播放的 variation 最高优先级
 *
 * 拉伸比例: ratio = 当前BPM / 循环包BPM
 */
class DrumLoopPlayer(
    private val context: Context,
    private val pushJs: (String) -> Unit,
    private val nativeRenderStart: (String, Int, Float, Int) -> Boolean,
    private val nativeRenderFeed: (String, ShortArray, Int) -> Long,
    private val nativeRenderFinish: (String, Int) -> Boolean,
    private val nativeSgsmStart: (String, Int, Float, Int) -> Boolean,
    private val nativeSgsmFeed: (String, ShortArray, Int) -> Long,
    private val nativeSgsmFinish: (String, Int) -> Boolean,
    private val nativeRenderCancelAll: () -> Unit,
    private val nativePlay: (String) -> Unit,
    private val nativeStop: () -> Unit,
    private val nativeClear: () -> Unit,
    private val getCurrentBpm: () -> Double
) {

    companion object {
        private const val TAG = "DrumLoopPlayer"
        private const val DRUM_LOOPS_DIR = "drumloops"
        private const val CHUNK_FRAMES = 8192         // 每次解码块帧数 (~0.17s)
        private const val PRE_ROLL_SECONDS = 0.5      // 高质量预热时长 (输出侧)

        const val STATE_NONE = 0        // 未渲染
        const val STATE_PRE_ROLLED = 1  // 高质量已渲染 0.5s
        const val STATE_DONE = 2        // 高质量完整渲染
    }

    private val loadGeneration = AtomicInteger(0)     // 选择包代数
    private val stretchGeneration = AtomicInteger(0)  // 渲染批次代数
    private val onDemandGen = AtomicInteger(0)        // 按需渲染请求代数

    private val opusData = mutableMapOf<String, ByteArray>()
    private var metaBpm = 0.0
    private var metaSampleRate = 48000
    private var folderLoaded = false

    @Volatile private var currentBpm = 0.0
    @Volatile private var playingVariation: String? = null

    private val stateLock = Object()
    private val decodeSessions = mutableMapOf<String, OpusDecoder>()
    private val states = mutableMapOf<String, Int>()          // variation → 0/1/2
    private val claims = mutableMapOf<String, Int>()          // variation → 认领 token
    private val startedStreams = mutableSetOf<String>()       // 已 nativeRenderStart (sbsms)
    private val startedSgsm = mutableSetOf<String>()          // 已 nativeSgsmStart
    private val sgsmDone = mutableSetOf<String>()             // sgsm 已完成渲染
    @Volatile private var totalInPass = 0
    @Volatile private var readyCount = 0
    @Volatile private var doneCount = 0

    // 三线程
    private val preRollExec = Executors.newSingleThreadExecutor { r ->
        Thread(r, "DrumPreRoll").apply { isDaemon = true }
    }
    private val fullExec = Executors.newSingleThreadExecutor { r ->
        Thread(r, "DrumFullRender").apply { isDaemon = true }
    }
    private val onDemandExec = Executors.newSingleThreadExecutor { r ->
        Thread(r, "DrumOnDemand").apply { isDaemon = true }
    }

    /** 拉伸比例 = 当前BPM / 循环包BPM */
    fun stretchRatio(currentBpm: Double, loopBpm: Double): Float {
        if (loopBpm <= 0.0) return 1.0f
        return (currentBpm / loopBpm).toFloat().coerceIn(0.25f, 4.0f)
    }

    // ================= 列表 / 选择循环包 =================

    fun getFolderListJson(): String {
        val arr = JSONArray()
        try {
            context.assets.list(DRUM_LOOPS_DIR)?.forEach { entry ->
                if (!entry.contains('.')) arr.put(entry)
            }
        } catch (e: Exception) {
            Log.e(TAG, "list drumloops failed", e)
        }
        return arr.toString()
    }

    fun selectFolder(folder: String) {
        val gen = loadGeneration.incrementAndGet()
        stretchGeneration.incrementAndGet()
        folderLoaded = false
        playingVariation = null
        nativeStop()
        nativeClear()
        nativeRenderCancelAll()
        releaseAllSessions()
        Thread {
            try {
                loadFolder(folder, gen)
            } catch (e: Exception) {
                Log.e(TAG, "loadFolder failed: $folder", e)
                pushJs("onDrumLoopError('${jsEscape(e.message ?: "加载失败")}')")
            }
        }.start()
    }

    private fun loadFolder(folder: String, gen: Int) {
        val assets = context.assets
        val base = "$DRUM_LOOPS_DIR/$folder"
        val entries = assets.list(base) ?: emptyArray()

        // 1. 元数据
        val jsonText = assets.open("$base/$folder.json").bufferedReader().use { it.readText() }
        val meta = JSONObject(jsonText)
        val variations = meta.getJSONObject("variations")
        val bpm = meta.optDouble("bpm", 0.0)
        val ts = meta.optString("time_signature", "4/4")
        val beatsPerBar = ts.split("/").firstOrNull()?.trim()?.toIntOrNull() ?: 4

        val arr = JSONArray()
        val keys = variations.keys()
        while (keys.hasNext()) {
            val key = keys.next()
            val v = variations.getJSONObject(key)
            val duration = v.optDouble("duration", 0.0)
            val bars = if (bpm > 0 && duration > 0) {
                Math.round(duration * bpm / 60.0 / beatsPerBar).toInt().coerceAtLeast(1)
            } else 1
            arr.put(JSONObject()
                .put("key", key)
                .put("category", categoryOf(key))
                .put("index", indexOf(key))
                .put("bars", bars))
        }
        pushJs("onDrumLoopMeta('" + jsEscape(JSONObject()
            .put("name", folder)
            .put("bpm", bpm)
            .put("variations", arr)
            .toString()) + "')")

        // 2. 载入全部 opus 字节
        synchronized(opusData) { opusData.clear() }
        metaBpm = bpm
        metaSampleRate = meta.optInt("sample_rate", 48000)
        val opusFiles = entries.filter { it.lowercase().endsWith(".opus") }.sorted()
        var done = 0
        for (f in opusFiles) {
            if (gen != loadGeneration.get()) return
            val key = f.removeSuffix(".opus").removeSuffix(".OPUS")
            val bytes = assets.open("$base/$f").use { it.readBytes() }
            synchronized(opusData) { opusData[key] = bytes }
            done++
            pushJs("onDrumLoopProgress($done, ${opusFiles.size})")
        }
        if (gen != loadGeneration.get()) return
        folderLoaded = true
        pushJs("onDrumLoopReady()")
        startStretchPass(null)
    }

    // ================= 渲染批次 (双引擎三线程) =================

    private fun startStretchPass(priority: String?) {
        val gen = stretchGeneration.incrementAndGet()
        val bpm = getCurrentBpm()
        currentBpm = bpm
        val ratio = stretchRatio(bpm, metaBpm)
        Log.i(TAG, "stretch pass gen=$gen bpm=$bpm loopBpm=$metaBpm ratio=$ratio priority=$priority")

        nativeRenderCancelAll()
        releaseAllSessions()

        val keys = synchronized(opusData) { opusData.keys.toList() }
        val ordered = keys.sortedWith(compareBy({ indexIntOf(it) }, { categoryRankOf(it) }, { it }))
        val order = if (priority != null && ordered.contains(priority))
            listOf(priority) + ordered.filter { it != priority } else ordered

        synchronized(stateLock) {
            totalInPass = ordered.size
            readyCount = 0
            doneCount = 0
            states.clear()
            claims.clear()
            startedStreams.clear()
            startedSgsm.clear()
            sgsmDone.clear()
        }
        pushJs("onDrumLoopStretchReset($totalInPass)")

        // 线程1: 按序号顺序高质量预热全部 variation (状态 0→1)
        for (k in order) {
            preRollExec.execute { safeTask(k) { taskPreRoll(k, ratio, gen, order, priority) } }
        }
    }

    /** 线程1: 高质量渲染前 0.5s (状态 0→1) */
    private fun taskPreRoll(name: String, ratio: Float, gen: Int, order: List<String>, priority: String?) {
        if (gen != stretchGeneration.get()) return
        val dec = getOrCreateDecoder(name) ?: return
        if (!nativeRenderStart(name, dec.sampleRate, ratio, gen)) {
            Log.w(TAG, "renderStart failed: $name")
            return
        }
        val preRollFrames = (dec.sampleRate * PRE_ROLL_SECONDS).toLong()  // 0.5s 输出
        var progress = 0L
        var eof = false
        while (progress < preRollFrames && gen == stretchGeneration.get()) {
            val chunk = dec.nextChunk(CHUNK_FRAMES) ?: run { eof = true; break }
            progress = nativeRenderFeed(name, chunk, gen)
            if (progress < 0) return  // 批次已过期
        }
        if (gen != stretchGeneration.get()) return

        if (eof) {
            // 音频短于 0.5s: 直接完整渲染
            if (nativeRenderFinish(name, gen)) {
                var rc = 0
                var dc = 0
                synchronized(stateLock) {
                    states[name] = STATE_DONE
                    doneCount++
                    dc = doneCount
                    readyCount++
                    rc = readyCount
                }
                pushJs("onDrumLoopPreRoll('$name',$rc,$totalInPass)")
                pushJs("onDrumLoopStretched('$name',$dc,$totalInPass)")
                if (dc >= totalInPass) pushJs("onDrumLoopStretchDone()")
                if (rc >= totalInPass) submitAllFullRenders(order, gen)
            }
            removeSession(name)
            return
        }

        // 预热完成 → 状态1
        var rc = 0
        synchronized(stateLock) {
            states[name] = STATE_PRE_ROLLED
            readyCount++
            rc = readyCount
        }
        pushJs("onDrumLoopPreRoll('$name',$rc,$totalInPass)")

        // 正在播放的 variation (BPM 改变): 立即完整渲染 (按需线程, 最高优先级)
        if (name == priority) submitOnDemand(name)

        // 全部预热完成 → 线程2 按序号顺序完整渲染
        if (rc >= totalInPass) submitAllFullRenders(order, gen)
    }

    private fun submitAllFullRenders(order: List<String>, gen: Int) {
        for (k in order) {
            fullExec.execute { safeTask(k) { taskFullRender(k, gen) } }
        }
    }

    /** 线程2: 高质量完整渲染 (状态 1→2) */
    private fun taskFullRender(name: String, gen: Int) {
        if (gen != stretchGeneration.get()) return
        val token = gen
        synchronized(stateLock) {
            if (claims.containsKey(name) || sgsmDone.contains(name)) return
            val st = states[name] ?: STATE_NONE
            if (st == STATE_DONE) return
            claims[name] = token
        }
        var completed = false
        try {
            val dec = getOrCreateDecoder(name)
            if (dec != null) {
                while (gen == stretchGeneration.get()) {
                    val chunk = dec.nextChunk(CHUNK_FRAMES) ?: break
                    if (nativeRenderFeed(name, chunk, gen) < 0) return
                }
            }
            if (gen != stretchGeneration.get()) return
            if (!nativeRenderFinish(name, gen)) {
                pushJs("onDrumLoopError('渲染失败: $name')")
                return
            }
            var dc = 0
            synchronized(stateLock) {
                states[name] = STATE_DONE
                doneCount++
                dc = doneCount
            }
            completed = true
            pushJs("onDrumLoopStretched('$name',$dc,$totalInPass)")
            if (dc >= totalInPass) pushJs("onDrumLoopStretchDone()")
        } finally {
            synchronized(stateLock) { if (claims[name] == token) claims.remove(name) }
            if (completed) removeSession(name)
        }
    }

    // ================= 按需渲染 (线程3, 双模式) =================

    /** 高质量续渲染 (状态1 点击 / BPM 改变的正在播放 variation) */
    private fun submitOnDemand(name: String) {
        val token = -onDemandGen.incrementAndGet()
        synchronized(stateLock) {
            if (claims.containsKey(name)) return
            claims[name] = token
        }
        onDemandExec.execute { safeTask(name) { taskOnDemandRender(name, token) } }
    }

    private fun taskOnDemandRender(name: String, token: Int) {
        val gen = stretchGeneration.get()
        var completed = false
        try {
            ensureRenderStarted(name)
            val dec = getOrCreateDecoder(name) ?: return
            while (token == -onDemandGen.get() && gen == stretchGeneration.get()) {
                val chunk = dec.nextChunk(CHUNK_FRAMES) ?: break
                if (nativeRenderFeed(name, chunk, gen) < 0) return
            }
            if (token != -onDemandGen.get() || gen != stretchGeneration.get()) return
            if (!nativeRenderFinish(name, gen)) {
                pushJs("onDrumLoopError('渲染失败: $name')")
                return
            }
            var dc = 0
            synchronized(stateLock) {
                states[name] = STATE_DONE
                doneCount++
                dc = doneCount
            }
            completed = true
            pushJs("onDrumLoopStretched('$name',$dc,$totalInPass)")
            if (dc >= totalInPass) pushJs("onDrumLoopStretchDone()")
        } finally {
            synchronized(stateLock) { if (claims[name] == token) claims.remove(name) }
            if (completed) removeSession(name)
        }
    }

    /** 低质量低延迟渲染 (状态0 点击): Signalsmith 流式, 几乎立即出声 */
    private fun submitSgsmOnDemand(name: String) {
        val token = -onDemandGen.incrementAndGet()
        synchronized(stateLock) {
            if (sgsmDone.contains(name) || claims.containsKey(name)) return
            claims[name] = token
        }
        onDemandExec.execute { safeTask(name) { taskOnDemandSgsm(name, token) } }
    }

    private fun taskOnDemandSgsm(name: String, token: Int) {
        val gen = stretchGeneration.get()
        var completed = false
        try {
            ensureSgsmStarted(name)
            val dec = getOrCreateDecoder(name) ?: return
            while (token == -onDemandGen.get() && gen == stretchGeneration.get()) {
                val chunk = dec.nextChunk(CHUNK_FRAMES) ?: break
                if (nativeSgsmFeed(name, chunk, gen) < 0) return
            }
            if (token != -onDemandGen.get() || gen != stretchGeneration.get()) return
            if (!nativeSgsmFinish(name, gen)) {
                pushJs("onDrumLoopError('渲染失败: $name')")
                return
            }
            synchronized(stateLock) { sgsmDone.add(name) }
            completed = true
        } finally {
            synchronized(stateLock) { if (claims[name] == token) claims.remove(name) }
            if (completed) removeSession(name)
        }
    }

    // ================= 播放 =================

    fun play(variation: String) {
        playingVariation = variation
        val state = synchronized(stateLock) { states[variation] ?: STATE_NONE }
        when (state) {
            STATE_DONE -> {
                // 状态2: 直接播放内存中的完整高质量音频
                nativePlay(variation)
            }
            STATE_PRE_ROLLED -> {
                // 状态1: 播放已渲染的 0.5s, 同时高质量续渲染
                ensureRenderStarted(variation)
                nativePlay(variation)
                submitOnDemand(variation)
            }
            else -> {
                // 状态0: Signalsmith 低质量引擎立即流式渲染播放 (低延迟)
                ensureSgsmStarted(variation)
                nativePlay(variation)
                submitSgsmOnDemand(variation)
            }
        }
        pushJs("onDrumLoopPlaying('$variation')")
    }

    fun stop() {
        playingVariation = null
        nativeStop()
    }

    /** BPM 变化回调 (MainActivity 轮询调用) */
    fun onBpmChanged(bpm: Double) {
        if (!folderLoaded) return
        if (Math.abs(bpm - currentBpm) < 0.05) return
        currentBpm = bpm
        startStretchPass(playingVariation)
    }

    // ================= 启动辅助 =================

    /** 确保本批次已为该 variation 启动 sbsms 渲染流 (幂等) */
    private fun ensureRenderStarted(name: String): Boolean {
        synchronized(stateLock) {
            if (startedStreams.contains(name)) return true
            startedStreams.add(name)
        }
        val ratio = stretchRatio(currentBpm, metaBpm)
        return nativeRenderStart(name, metaSampleRate, ratio, stretchGeneration.get())
    }

    /** 确保本批次已为该 variation 启动 Signalsmith 渲染流 (幂等) */
    private fun ensureSgsmStarted(name: String): Boolean {
        synchronized(stateLock) {
            if (startedSgsm.contains(name)) return true
            startedSgsm.add(name)
        }
        val ratio = stretchRatio(currentBpm, metaBpm)
        return nativeSgsmStart(name, metaSampleRate, ratio, stretchGeneration.get())
    }

    // ================= 解码会话 =================

    private fun getOrCreateDecoder(name: String): OpusDecoder? {
        synchronized(stateLock) {
            decodeSessions[name]?.let { return it }
        }
        val bytes = synchronized(opusData) { opusData[name] } ?: return null
        val dec = try {
            OpusDecoder(bytes)
        } catch (e: Exception) {
            Log.e(TAG, "create decoder failed: $name", e)
            return null
        }
        synchronized(stateLock) { decodeSessions[name] = dec }
        return dec
    }

    private fun removeSession(name: String) {
        synchronized(stateLock) { decodeSessions.remove(name) }
    }

    private fun releaseAllSessions() {
        val list = synchronized(stateLock) {
            val l = decodeSessions.values.toList()
            decodeSessions.clear()
            l
        }
        list.forEach { runCatching { it.release() } }
    }

    private fun safeTask(name: String, body: () -> Unit) {
        try {
            body()
        } catch (e: Exception) {
            Log.e(TAG, "task failed: $name", e)
        }
    }

    // ================= Opus 解码器 (可暂停/恢复) =================

    private class ByteArrayDataSource(private val data: ByteArray) : MediaDataSource() {
        override fun readAt(position: Long, buffer: ByteArray, offset: Int, size: Int): Int {
            if (position >= data.size) return -1
            val len = minOf(size, data.size - position.toInt())
            System.arraycopy(data, position.toInt(), buffer, offset, len)
            return len
        }
        override fun getSize(): Long = data.size.toLong()
        override fun close() {}
    }

    /** 可恢复的 opus 解码器: nextChunk 逐块取 PCM, null 表示结束 */
    private class OpusDecoder(data: ByteArray) {
        private val extractor = MediaExtractor()
        private val codec: MediaCodec
        private val info = MediaCodec.BufferInfo()
        private val chunkBytes = ByteArrayOutputStream()
        val sampleRate: Int
        private var inputDone = false
        private var outputDone = false
        private var released = false

        init {
            extractor.setDataSource(ByteArrayDataSource(data))
            var fmt: MediaFormat? = null
            for (i in 0 until extractor.trackCount) {
                val f = extractor.getTrackFormat(i)
                val mime = f.getString(MediaFormat.KEY_MIME) ?: continue
                if (mime.startsWith("audio/")) { fmt = f; extractor.selectTrack(i); break }
            }
            val format = fmt ?: throw IllegalStateException("no audio track")
            sampleRate = if (format.containsKey(MediaFormat.KEY_SAMPLE_RATE))
                format.getInteger(MediaFormat.KEY_SAMPLE_RATE) else 48000
            codec = MediaCodec.createDecoderByType(format.getString(MediaFormat.KEY_MIME)!!)
            codec.configure(format, null, null, 0)
            codec.start()
        }

        fun nextChunk(maxFrames: Int): ShortArray? {
            if (released) return null
            if (outputDone) return null
            chunkBytes.reset()
            val maxBytes = maxFrames * 2 * 2  // int16 交错立体声
            while (chunkBytes.size() < maxBytes && !outputDone) {
                if (!inputDone) {
                    val inIdx = codec.dequeueInputBuffer(5000)
                    if (inIdx >= 0) {
                        val buf = codec.getInputBuffer(inIdx)!!
                        val n = extractor.readSampleData(buf, 0)
                        if (n < 0) {
                            codec.queueInputBuffer(inIdx, 0, 0, 0, MediaCodec.BUFFER_FLAG_END_OF_STREAM)
                            inputDone = true
                        } else {
                            codec.queueInputBuffer(inIdx, 0, n, extractor.sampleTime, 0)
                            extractor.advance()
                        }
                    }
                }
                when (val outIdx = codec.dequeueOutputBuffer(info, 5000)) {
                    MediaCodec.INFO_OUTPUT_FORMAT_CHANGED, MediaCodec.INFO_TRY_AGAIN_LATER -> {}
                    else -> {
                        if (outIdx >= 0) {
                            val buf = codec.getOutputBuffer(outIdx)!!
                            if (info.size > 0) {
                                buf.position(info.offset)
                                buf.limit(info.offset + info.size)
                                val bytes = ByteArray(info.size)
                                buf.get(bytes)
                                chunkBytes.write(bytes)
                            }
                            codec.releaseOutputBuffer(outIdx, false)
                            if (info.flags and MediaCodec.BUFFER_FLAG_END_OF_STREAM != 0) outputDone = true
                        }
                    }
                }
            }
            val bytes = chunkBytes.toByteArray()
            if (bytes.isEmpty() && outputDone) return null
            val shorts = ShortArray(bytes.size / 2)
            for (i in shorts.indices) {
                val b0 = bytes[i * 2].toInt() and 0xFF
                val b1 = bytes[i * 2 + 1].toInt() and 0xFF
                shorts[i] = (b0 or (b1 shl 8)).toShort()
            }
            return shorts
        }

        fun release() {
            if (released) return
            released = true
            runCatching { codec.stop() }
            runCatching { codec.release() }
            runCatching { extractor.release() }
        }
    }

    // ================= variation 键解析 =================

    private fun categoryOf(key: String): String {
        val parts = key.split("_")
        val head = parts.firstOrNull() ?: return "other"
        return when (head) {
            "VERSE" -> if (parts.size > 2 && parts[1].matches(Regex("\\d+"))) "verse2" else "verse"
            "PRE" -> "prechorus"
            "CHORUS" -> "chorus"
            "FILLS" -> "fill"
            "INTRO" -> "intro"
            "BRIDGE" -> "bridge"
            "ENDINGS" -> "ending"
            "PICKUPS" -> "pickup"
            else -> "other"
        }
    }

    private fun indexOf(key: String): String {
        val parts = key.split("_")
        return parts.lastOrNull()?.takeIf { it.matches(Regex("\\d+")) } ?: ""
    }

    private fun indexIntOf(key: String): Int = indexOf(key).toIntOrNull() ?: Int.MAX_VALUE

    private fun categoryRankOf(key: String): Int = when (categoryOf(key)) {
        "verse" -> 0
        "verse2" -> 1
        "prechorus" -> 2
        "chorus" -> 3
        "fill" -> 4
        "intro" -> 5
        "bridge" -> 6
        "ending" -> 7
        "pickup" -> 8
        else -> 9
    }

    private fun jsEscape(s: String): String = s.replace("\\", "\\\\").replace("'", "\\'")
}
