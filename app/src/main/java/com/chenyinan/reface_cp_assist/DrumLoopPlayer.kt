package com.chenyinan.reface_cp_assist

import android.content.Context
import android.media.MediaCodec
import android.media.MediaDataSource
import android.media.MediaExtractor
import android.media.MediaFormat
import android.os.Process
import android.util.Log
import org.json.JSONArray
import org.json.JSONObject
import java.io.ByteArrayOutputStream
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicInteger

/**
 * 鼓循环管理 (双引擎、独立高质量调度线程):
 *
 * 状态: 0=完全未渲染; 1=头0.5s; 2=头0.5s+最后一小节; 3=完整。
 * Signalsmith 只负责状态0的即时播放和 cut-point 变速；SBSMS 按
 * 当前 variation → 目标 variation → 其他 variation 的优先级独立预渲染。
 *
 * 拉伸比例: ratio = 当前BPM / 循环包BPM
 */
class DrumLoopPlayer(
    private val context: Context,
    private val pushJs: (String) -> Unit,
    private val nativeRenderStart: (String, Int, Float, Int, Double, Double) -> Boolean,
    private val nativeRenderFeed: (String, ShortArray, Int) -> Long,
    private val nativeRenderFinishStep: (String, Int) -> Long,
    private val nativeRenderCommit: (String, Int, Int) -> Boolean,
    private val nativeSgsmStart: (String, Int, Float, Int, Double, Double) -> Boolean,
    private val nativeSgsmFeed: (String, ShortArray, Int) -> Long,
    private val nativeSgsmFinishStep: (String, Int) -> Long,
    private val nativeRequestSwitch: (String) -> Unit,
    private val nativeFadeOut: (Int) -> Unit,
    private val nativeSyncBeat: () -> Unit,
    private val nativeRenderCancelAll: () -> Unit,
    private val nativePlay: (String) -> Unit,
    private val nativePlayAt: (String, Double) -> Unit,
    private val nativeStop: () -> Unit,
    private val nativeClear: () -> Unit,
    private val nativeGetPlaybackPosition: (String) -> Double,
    private val nativeArmTempoCut: (
        String, Int, Double, DoubleArray, DoubleArray, Double
    ) -> Boolean,
    private val nativeCancelTempoCut: () -> Unit,
    private val nativeArmAutoFillJump: (String, Int, Double, Double) -> Boolean,
    private val nativeCancelAutoFillJump: () -> Unit,
    private val nativeStartOneShot: (
        String, Double, Int, String, Boolean, Double, Double
    ) -> Boolean,
    private val nativeUpdateOneShotNext: (String) -> Boolean,
    private val nativeCancelOneShot: () -> Unit,
    private val nativeSetBassMidi: (
        String, DoubleArray, IntArray, IntArray, IntArray, IntArray, IntArray
    ) -> Boolean,
    private val getCurrentBpm: () -> Double
) {

    internal data class AutoFillCutPoint(val time: Double, val rms: Double)

    internal data class AutoFillTransition(
        val sourceTime: Double,
        val destinationTime: Double,
        val delaySeconds: Double
    )

    companion object {
        private const val TAG = "DrumLoopPlayer"
        private const val DRUM_LOOPS_DIR = "drumloops"
        private const val CHUNK_FRAMES = 8192         // 每次解码块帧数 (~0.17s)
        private const val PRE_ROLL_SECONDS = 0.5      // 高质量预热时长 (输出侧)
        private const val MIN_CUT_LEAD_SECONDS = 0.250
        private const val AUTO_FILL_PHASE_TOLERANCE_SECONDS = 0.030
        private const val AUTO_FILL_FADE_SECONDS = 0.030
        private const val SPECIAL_START_WATERMARK_SECONDS = 0.250
        private const val SMART_FILL_FADE_MS = 30
        private const val INTRO_FADE_MS = 30
        private const val BREAK_FADE_MS = 10
        private const val ENDING_FADE_MS = 30
        private val MAIN_VARIATION_CATEGORIES =
            setOf("verse", "verse2", "prechorus", "chorus")

        const val STATE_NONE = 0        // 完全未渲染
        const val STATE_PRE_ROLLED = 1  // 高质量已渲染头部 0.5s
        const val STATE_TAIL_READY = 2  // 高质量已渲染头部 0.5s + 最后一小节
        const val STATE_DONE = 3        // 高质量完整渲染

        /** 原始时间点 → 指定 BPM 下的时间点。 */
        fun sourceTimeToTempoTime(sourceBpm: Double, targetBpm: Double, sourceTime: Double): Double {
            if (sourceBpm <= 0.0 || targetBpm <= 0.0) return sourceTime
            return sourceTime * sourceBpm / targetBpm
        }

        /** 指定 BPM 下的时间点 → 原始音频时间点。 */
        fun tempoTimeToSourceTime(sourceBpm: Double, targetBpm: Double, tempoTime: Double): Double {
            if (sourceBpm <= 0.0 || targetBpm <= 0.0) return tempoTime
            return tempoTime * targetBpm / sourceBpm
        }

        /** 判断源 MIDI 起音是否位于每小节第一拍；15ms 只吸收导出量化误差。 */
        internal fun isMeasureDownbeat(timeSeconds: Double, measureSeconds: Double): Boolean {
            if (!timeSeconds.isFinite() || !measureSeconds.isFinite() ||
                timeSeconds < 0.0 || measureSeconds <= 0.0
            ) return false
            val phase = timeSeconds % measureSeconds
            // 只吸收拍头之后的导出量化误差。拍头之前的音符属于上一小节的
            // pickup，不能因为它靠近循环边界而被误判成下一小节的重拍。
            return phase <= 0.015
        }

        /**
         * 第 1/3 拍附近的 C4 是和弦重触发锚点。允许前后一个三十二分音符
         * 的人性化偏移；该标记在载入 MIDI 时计算，播放线程只读取布尔值。
         */
        internal fun isBassRetriggerStrongBeat(
            timeSeconds: Double,
            measureSeconds: Double,
            beatsPerBar: Int
        ): Boolean {
            if (!timeSeconds.isFinite() || !measureSeconds.isFinite() ||
                timeSeconds < 0.0 || measureSeconds <= 0.0 || beatsPerBar <= 0
            ) return false
            val phase = timeSeconds % measureSeconds
            val beatSeconds = measureSeconds / beatsPerBar.toDouble()
            val tolerance = beatSeconds / 8.0 // 一个三十二分音符
            val distanceToBeatOne = minOf(phase, measureSeconds - phase)
            if (distanceToBeatOne <= tolerance) return true
            if (beatsPerBar < 3) return false
            val beatThree = beatSeconds * 2.0
            return kotlin.math.abs(phase - beatThree) <= tolerance
        }

        /**
         * 将当前主 variation 在 RMS 排名中的分位映射到候选组的 RMS 排名。
         * 两端严格对齐：最轻映射最轻、最响映射最响；数量不同时取最近的候选排名。
         */
        internal fun findRankMappedVariation(
            current: String,
            mainVariations: List<Pair<String, Double>>,
            candidates: List<Pair<String, Double>>
        ): String? {
            val mains = mainVariations.asSequence()
                .filter { it.second.isFinite() }
                .sortedWith(compareBy<Pair<String, Double>>({ it.second }, { it.first }))
                .toList()
            val fills = candidates.asSequence()
                .filter { it.second.isFinite() }
                .sortedWith(compareBy<Pair<String, Double>>({ it.second }, { it.first }))
                .toList()
            if (mains.isEmpty() || fills.isEmpty()) return null
            val mainRank = mains.indexOfFirst { it.first == current }
            if (mainRank < 0) return null
            if (mains.size == 1 || fills.size == 1) return fills.first().first
            val mappedRank = mainRank.toDouble() * (fills.size - 1).toDouble() /
                (mains.size - 1).toDouble()
            val fillRank = kotlin.math.floor(mappedRank + 0.5).toInt()
                .coerceIn(0, fills.lastIndex)
            return fills[fillRank].first
        }

        /**
         * 从当前位置一直搜索到本小节结束前的最后一个安全 A 切点，并与最后一小节
         * 同相位 ±30 ms 内的 B 切点配对。选择最早可执行的一对，让点击尽快响应；
         * 两端均必须留出完整的 30 ms constant-power 淡化时间。
         */
        internal fun findAutoFillTransition(
            cuts: List<AutoFillCutPoint>,
            positionSec: Double,
            durationSec: Double,
            measureSec: Double
        ): AutoFillTransition? {
            if (cuts.isEmpty() || !positionSec.isFinite() || durationSec <= 0.0 ||
                measureSec <= 0.0 || durationSec <= measureSec
            ) return null

            val position = ((positionSec % durationSec) + durationSec) % durationSec
            val lastMeasureStart = durationSec - measureSec
            // 已经在最后一小节时只保留原来的下一小节切换，不重复跳转。
            if (position + 1e-6 >= lastMeasureStart) return null

            val measureIndex = kotlin.math.floor(position / measureSec).toInt()
            val measureStart = measureIndex * measureSec
            val measureEnd = minOf(measureStart + measureSec, lastMeasureStart)
            val latestSourceCut = measureEnd - AUTO_FILL_FADE_SECONDS
            if (latestSourceCut + 1e-6 < position) return null

            var best: AutoFillTransition? = null
            var bestSourceTime = Double.POSITIVE_INFINITY
            var bestScore = Double.POSITIVE_INFINITY
            var bestPhaseError = Double.POSITIVE_INFINITY
            for (a in cuts) {
                if (a.time + 1e-6 < position) continue
                if (a.time - 1e-6 > latestSourceCut) break
                val phaseA = a.time - measureStart
                if (phaseA < 0.0 || phaseA >= measureSec) continue
                val expectedB = lastMeasureStart + phaseA
                for (b in cuts) {
                    if (b.time + 1e-6 < lastMeasureStart) continue
                    val phaseError = kotlin.math.abs(b.time - expectedB)
                    if (phaseError > AUTO_FILL_PHASE_TOLERANCE_SECONDS + 1e-6) continue
                    // 参考算法取 A/B 低能量点在小节相位上的中点，并让两条音频
                    // 都从该相位交叉淡化，从而保持跳转前后的 Transport 相位完全一致。
                    val phaseB = b.time - lastMeasureStart
                    val transitionPhase = (phaseA + phaseB) * 0.5
                    val sourceTime = measureStart + transitionPhase
                    val destinationTime = lastMeasureStart + transitionPhase
                    if (sourceTime + 1e-6 < position ||
                        sourceTime + AUTO_FILL_FADE_SECONDS > measureEnd + 1e-6 ||
                        destinationTime + AUTO_FILL_FADE_SECONDS > durationSec + 1e-6
                    ) continue
                    val score = a.rms.coerceAtLeast(0.0) + b.rms.coerceAtLeast(0.0)
                    if (sourceTime < bestSourceTime - 1e-6 ||
                        (kotlin.math.abs(sourceTime - bestSourceTime) <= 1e-6 &&
                            (score < bestScore - 1e-12 ||
                                (kotlin.math.abs(score - bestScore) <= 1e-12 &&
                                    phaseError < bestPhaseError)))
                    ) {
                        bestSourceTime = sourceTime
                        bestScore = score
                        bestPhaseError = phaseError
                        best = AutoFillTransition(
                            sourceTime = sourceTime,
                            destinationTime = destinationTime,
                            delaySeconds = (sourceTime - position).coerceAtLeast(0.0)
                        )
                    }
                }
            }
            return best
        }
    }

    private data class StretchPass(
        val gen: Int,
        val ratio: Float,
        val order: List<String>,
        val priority: String?,
        val bpm: Double
    )

    private data class CutSelection(
        val cutAtCurrentTempo: Double,
        val delaySeconds: Double,
        val sourceTime: Double,
        val targetTime: Double,
        val targetDuration: Double,
        /** 每个候选 cut point 相对目标旋转缓冲区开头的物理位置。 */
        val targetOffsetsSec: DoubleArray,
        /** 当前候选未准备好时，到下一个候选 cut point 的旧速度时间。 */
        val nextDelaysSec: DoubleArray
    )

    private enum class HqLayout { HEAD, ROTATED_TAIL }

    private data class HqSession(
        var decoder: PcmChunkSource?,
        val gen: Int,
        val layout: HqLayout,
        val logicalOriginSec: Double,
        val sampleRate: Int,
        val rotationSplitUs: Long,
        var resumeSourceUs: Long,
        var resumeWrapped: Boolean,
        var progressFrames: Long = 0L,
        var inputEnded: Boolean = false
    )

    private val loadGeneration = AtomicInteger(0)     // 选择包代数
    private val stretchGeneration = AtomicInteger(0)  // 渲染批次代数
    private val sgsmTokenCounter = AtomicInteger(0)   // Signalsmith 任务 token

    private val opusData = mutableMapOf<String, ByteArray>()
    private val variationKeys = mutableListOf<String>()
    private val variationIndex = mutableMapOf<String, Int>()
    private val sourceDurations = mutableListOf<Double>()
    private val sourceCutPoints = mutableListOf<List<AutoFillCutPoint>>()
    private val averageRmsByVariation = mutableMapOf<String, Double>()
    private var currentCutPoints = mutableListOf<List<AutoFillCutPoint>>()
    private var currentDurations = mutableListOf<Double>()
    private var metaBpm = 0.0
    private var metaSampleRate = 48000
    private var beatsPerBar = 4
    @Volatile private var folderLoaded = false

    @Volatile private var currentBpm = 0.0
    @Volatile private var activePlaybackBpm = 0.0
    @Volatile private var playingVariation: String? = null
    private val bpmTransitionLock = Any()
    @Volatile private var targetBpm = -1.0
    private var bpmTransitionActive = false
    private var bpmTransitionTarget = -1.0
    private var bpmTransitionGen = -1
    private var bpmTransitionName: String? = null

    private val stateLock = Object()
    private val hqSessions = mutableMapOf<String, HqSession>()
    private val states = mutableMapOf<String, Int>()          // variation → 0/1/2/3
    private val sgsmClaims = mutableMapOf<String, Int>()      // 低延迟渲染独立 token/解码器，可与高质量预热并行
    private val startedSgsm = mutableSetOf<String>()          // 已 nativeSgsmStart
    private val sgsmDone = mutableSetOf<String>()             // sgsm 已完成渲染
    private val liveSgsmKeys = mutableSetOf<String>()          // 最多保留当前播放流 + 下一目标流
    private val sgsmReadyCallbacks = mutableMapOf<String, MutableList<() -> Unit>>()
    private val sgsmPlayableCallbacks = mutableMapOf<String, MutableList<() -> Unit>>()
    private val sgsmLogicalOrigins = mutableMapOf<String, Double>()
    @Volatile private var activePlaybackGeneration = -1
    private val hqPlanRevision = AtomicInteger(0)
    @Volatile private var activeStretchPass: StretchPass? = null
    @Volatile private var totalInPass = 0
    @Volatile private var readyCount = 0
    @Volatile private var doneCount = 0

    private enum class OneShotKind(val jsName: String) {
        SMART_FILL("fill"), INTRO("intro"), BREAK("break"),
        BREAK_TARGET("break"), ENDING("ending")
    }

    @Volatile private var oneShotKind: OneShotKind? = null
    @Volatile private var oneShotVariation: String? = null
    @Volatile private var introSelected = false
    @Volatile private var oneShotStarted = false
    @Volatile private var oneShotAudioStarted = false
    @Volatile private var oneShotFixedReturn: String? = null
    @Volatile private var oneShotPhaseReference: String? = null
    @Volatile private var oneShotStartInLastMeasure = false
    @Volatile private var oneShotStartDeadlineNs = 0L
    @Volatile private var deferredOneShotBpm = -1.0
    private val oneShotLock = Any()
    private val oneShotSerial = AtomicInteger(0)

    /**
     * 拉伸计算与 Oboe 播放本来就在不同线程；这里进一步降低所有渲染线程的 Linux nice 值，
     * 让实时音频回调在 CPU 紧张时优先获得时间片。按需线程略高于批量预热，保证拍头前能准备好目标。
     */
    private fun renderWorker(name: String, androidPriority: Int, task: Runnable) =
        Thread({
            runCatching { Process.setThreadPriority(androidPriority) }
                .onFailure { Log.w(TAG, "set thread priority failed: $name", it) }
            task.run()
        }, name).apply { isDaemon = true }

    // SBSMS 的所有阶段由单一后台队列严格按优先级串行执行；Signalsmith 另用按需队列。
    private val hqExec = Executors.newSingleThreadExecutor { r ->
        renderWorker("DrumHighQuality", Process.THREAD_PRIORITY_BACKGROUND, r)
    }
    private val onDemandExec = Executors.newFixedThreadPool(2) { r ->
        renderWorker("DrumOnDemand", Process.THREAD_PRIORITY_DEFAULT + 4, r)
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
        targetVariation = null
        oneShotSerial.incrementAndGet()
        oneShotKind = null
        oneShotVariation = null
        introSelected = false
        oneShotStarted = false
        oneShotAudioStarted = false
        oneShotFixedReturn = null
        oneShotPhaseReference = null
        oneShotStartInLastMeasure = false
        oneShotStartDeadlineNs = 0L
        deferredOneShotBpm = -1.0
        nativeCancelTempoCut()
        nativeCancelAutoFillJump()
        nativeCancelOneShot()
        synchronized(bpmTransitionLock) {
            targetBpm = -1.0
            bpmTransitionActive = false
            bpmTransitionGen = -1
            bpmTransitionName = null
        }
        hqPlanRevision.incrementAndGet()
        activeStretchPass = null
        activePlaybackGeneration = -1
        nativeStop()
        nativeClear()
        nativeRenderCancelAll()
        releaseAllHqSessions()
        synchronized(stateLock) {
            sgsmClaims.clear()
            startedSgsm.clear()
            sgsmDone.clear()
            liveSgsmKeys.clear()
            sgsmReadyCallbacks.clear()
            sgsmPlayableCallbacks.clear()
            sgsmLogicalOrigins.clear()
        }
        if (folder.isBlank()) {
            synchronized(opusData) { opusData.clear() }
            synchronized(stateLock) {
                states.clear()
                variationKeys.clear()
                variationIndex.clear()
                sourceDurations.clear()
                sourceCutPoints.clear()
                averageRmsByVariation.clear()
                currentCutPoints.clear()
                currentDurations.clear()
            }
            return
        }
        renderWorker("DrumAssetLoader", Process.THREAD_PRIORITY_BACKGROUND, Runnable {
            try {
                loadFolder(folder, gen)
            } catch (e: Exception) {
                Log.e(TAG, "loadFolder failed: $folder", e)
                pushJs("onDrumLoopError('${jsEscape(e.message ?: "加载失败")}')")
            }
        }).start()
    }

    private fun loadFolder(folder: String, gen: Int) {
        val assets = context.assets
        val base = "$DRUM_LOOPS_DIR/$folder"
        val entries = assets.list(base) ?: emptyArray()
        val opusFileByBaseName = entries.asSequence()
            .filter { it.substringAfterLast('.', "").equals("opus", ignoreCase = true) }
            .associateBy { it.substringBeforeLast('.').uppercase() }
        val midiFileByBaseName = entries.asSequence()
            .filter { it.substringAfterLast('.', "").equals("mid", ignoreCase = true) }
            .associateBy { it.substringBeforeLast('.').uppercase() }

        // 1. 元数据
        val jsonText = assets.open("$base/$folder.json").bufferedReader().use { it.readText() }
        val meta = JSONObject(jsonText)
        val variations = meta.getJSONObject("variations")
        val bpm = meta.optDouble("bpm", 0.0)
        val ts = meta.optString("time_signature", "4/4")
        val parsedBeatsPerBar = ts.split("/").firstOrNull()?.trim()?.toIntOrNull() ?: 4

        val arr = JSONArray()
        val keys = variations.keys()
        val keyList = mutableListOf<String>()
        val durationList = mutableListOf<Double>()
        val cutPointList = mutableListOf<List<AutoFillCutPoint>>()
        val averageRmsMap = mutableMapOf<String, Double>()
        val opusFileByJsonKey = mutableMapOf<String, String>()
        while (keys.hasNext()) {
            val key = keys.next()
            if (!isSupportedVariationKey(key)) {
                Log.d(TAG, "ignore unused variation: $key")
                continue
            }
            val opusFile = opusFileByBaseName[key.uppercase()]
            if (opusFile == null) {
                Log.w(TAG, "ignore variation without Opus asset: $key")
                continue
            }
            opusFileByJsonKey[key] = opusFile
            keyList.add(key)
            val v = variations.getJSONObject(key)
            val duration = v.optDouble("duration", 0.0)
            durationList.add(duration)
            val averageRms = v.optDouble("average_rms", Double.NaN)
            if (averageRms.isFinite()) averageRmsMap[key] = averageRms
            val cutsJson = v.optJSONArray("cut_points")
            val cuts = mutableListOf<AutoFillCutPoint>()
            if (cutsJson != null) {
                for (i in 0 until cutsJson.length()) {
                    val cutJson = cutsJson.optJSONObject(i) ?: continue
                    val cut = cutJson.optDouble("time", -1.0)
                    val rms = cutJson.optDouble("rms", 1.0).takeIf { it.isFinite() } ?: 1.0
                    if (cut >= 0.0 && (duration <= 0.0 || cut < duration)) {
                        cuts.add(AutoFillCutPoint(cut, rms))
                    }
                }
            }
            cutPointList.add(cuts.distinctBy { it.time }.sortedBy { it.time })
            val bars = if (bpm > 0 && duration > 0) {
                Math.round(duration * bpm / 60.0 / parsedBeatsPerBar).toInt().coerceAtLeast(1)
            } else 1
            arr.put(JSONObject()
                .put("key", key)
                .put("category", categoryOf(key))
                .put("index", indexOf(key))
                .put("bars", bars))
        }
        if (gen != loadGeneration.get()) return
        synchronized(stateLock) {
            variationKeys.clear()
            variationKeys.addAll(keyList)
            variationIndex.clear()
            keyList.forEachIndexed { index, key -> variationIndex[key] = index }
            sourceDurations.clear()
            sourceDurations.addAll(durationList)
            sourceCutPoints.clear()
            sourceCutPoints.addAll(cutPointList)
            averageRmsByVariation.clear()
            averageRmsByVariation.putAll(averageRmsMap)
        }
        pushJs("onDrumLoopMeta('" + jsEscape(JSONObject()
            .put("name", folder)
            .put("bpm", bpm)
            .put("variations", arr)
            .toString()) + "')")

        // 默认待播放: VERSE 类别第一个 (一般是 verse_01), 无则按 VERSE2/PRECHORUS/CHORUS 顺序
        val defPending = pickDefaultPending(keyList)
        if (gen != loadGeneration.get()) return
        targetVariation = defPending
        if (defPending != null) pushJs("onDrumLoopDefaultPending('$defPending')")

        // 2. 仅载入 JSON∩Opus 且实际会用到的 variation。
        synchronized(opusData) { opusData.clear() }
        metaBpm = bpm
        metaSampleRate = meta.optInt("sample_rate", 48000)
        beatsPerBar = parsedBeatsPerBar.coerceAtLeast(1)
        var done = 0
        for ((keyIndex, key) in keyList.withIndex()) {
            if (gen != loadGeneration.get()) return
            val f = opusFileByJsonKey[key] ?: continue
            val bytes = assets.open("$base/$f").use { it.readBytes() }
            synchronized(opusData) { opusData[key] = bytes }
            val sourceDuration = durationList.getOrNull(keyIndex) ?: 0.0
            val midiFile = midiFileByBaseName[key.uppercase()]
            if (midiFile != null && sourceDuration > 0.0) {
                runCatching {
                    val phrase = DrumLoopMidiParser.parse(
                        assets.open("$base/$midiFile").use { it.readBytes() }
                    )
                    val usable = phrase.events.filter {
                        it.timeSeconds >= 0.0 && it.timeSeconds <= sourceDuration + 0.100
                    }
                    val phases = DoubleArray(usable.size) { index ->
                        (usable[index].timeSeconds / sourceDuration).coerceIn(0.0, 1.0)
                    }
                    val velocities = IntArray(usable.size) { index ->
                        if (usable[index].noteOn) usable[index].velocity else 0
                    }
                    val measureSeconds = if (bpm > 0.0) {
                        60.0 * parsedBeatsPerBar.toDouble() / bpm
                    } else 0.0
                    val downbeats = IntArray(usable.size) { index ->
                        val event = usable[index]
                        if (event.noteOn && isMeasureDownbeat(
                                event.timeSeconds, measureSeconds
                            )) 1 else 0
                    }
                    val strongBeats = IntArray(usable.size) { index ->
                        val event = usable[index]
                        if (event.noteOn && isBassRetriggerStrongBeat(
                                event.timeSeconds, measureSeconds, parsedBeatsPerBar
                            )) 1 else 0
                    }
                    val noteKinds = IntArray(usable.size) { index -> usable[index].kind }
                    val phraseGroups = IntArray(usable.size) { index ->
                        usable[index].phraseGroup
                    }
                    check(nativeSetBassMidi(
                        key, phases, velocities, downbeats, strongBeats,
                        noteKinds, phraseGroups
                    )) {
                        "原生贝斯乐句注册失败"
                    }
                    Log.i(
                        TAG,
                        "bass MIDI loaded: $key events=${usable.size} " +
                            "C4=${phrase.c4NoteOnCount} C5=${phrase.c5NoteOnCount} " +
                            "connectedC5=${phrase.connectedC5NoteOnCount}"
                    )
                }.onFailure { error ->
                    nativeSetBassMidi(
                        key, DoubleArray(0), IntArray(0), IntArray(0),
                        IntArray(0), IntArray(0), IntArray(0)
                    )
                    Log.w(TAG, "ignore invalid bass MIDI: $midiFile", error)
                }
            } else {
                nativeSetBassMidi(
                    key, DoubleArray(0), IntArray(0), IntArray(0),
                    IntArray(0), IntArray(0), IntArray(0)
                )
                if (midiFile == null) Log.d(TAG, "no bass MIDI for variation: $key")
            }
            done++
            pushJs("onDrumLoopProgress($done, ${keyList.size})")
        }
        if (gen != loadGeneration.get()) return
        folderLoaded = true
        activePlaybackBpm = getCurrentBpm()
        recalculateCurrentCutPoints(activePlaybackBpm)
        pushJs("onDrumLoopReady()")
        startStretchPass(null)
    }

    /** 按指定速度重建全部 variation 的 cut point 二维列表及总时长列表。 */
    private fun recalculateCurrentCutPoints(bpm: Double) {
        val sourceCuts: List<List<AutoFillCutPoint>>
        val durations: List<Double>
        synchronized(stateLock) {
            sourceCuts = sourceCutPoints.map { it.toList() }
            durations = sourceDurations.toList()
        }
        val scaledCuts = sourceCuts.map { cuts ->
            cuts.map {
                AutoFillCutPoint(sourceTimeToTempoTime(metaBpm, bpm, it.time), it.rms)
            }
        }
        val scaledDurations = durations.map { sourceTimeToTempoTime(metaBpm, bpm, it) }
        synchronized(stateLock) {
            currentCutPoints = scaledCuts.toMutableList()
            currentDurations = scaledDurations.toMutableList()
        }
    }

    private fun logicalDuration(name: String, bpm: Double = currentBpm): Double {
        val source = synchronized(stateLock) {
            variationIndex[name]?.let { sourceDurations.getOrNull(it) }
        } ?: 0.0
        return sourceTimeToTempoTime(metaBpm, bpm, source)
    }

    private fun currentMeasureSeconds(): Double {
        val bpm = activePlaybackBpm.takeIf { it > 0.0 }
            ?: currentBpm.takeIf { it > 0.0 }
            ?: getCurrentBpm()
        return if (bpm > 0.0) beatsPerBar.coerceAtLeast(1) * 60.0 / bpm else 0.0
    }

    /**
     * 选择当前播放位置至少 250 ms 之后的首个 cut point，并生成之后一整轮的候选计划。
     * native 到点时若目标缓冲未达到安全水位，会沿该计划跳过本点而继续播放旧速度。
     */
    private fun selectNextTempoCut(
        name: String,
        positionSec: Double,
        fromBpm: Double,
        toBpm: Double
    ): CutSelection? {
        val timing = synchronized(stateLock) {
            val index = variationIndex[name] ?: return null
            Pair(currentCutPoints.getOrNull(index)?.toList().orEmpty(),
                currentDurations.getOrNull(index) ?: 0.0)
        }
        val cuts = timing.first.map { it.time }
        val duration = timing.second
        if (cuts.isEmpty() || duration <= 0.0) return null

        val pos = ((positionSec % duration) + duration) % duration
        val sortedCuts = cuts.asSequence()
            .filter { it.isFinite() }
            .map { ((it % duration) + duration) % duration }
            .sorted()
            .fold(mutableListOf<Double>()) { unique, value ->
                if (unique.isEmpty() || kotlin.math.abs(unique.last() - value) > 1e-6) {
                    unique.add(value)
                }
                unique
            }
        if (sortedCuts.isEmpty()) return null

        var firstIndex = -1
        var delay = Double.POSITIVE_INFINITY
        sortedCuts.forEachIndexed { index, cut ->
            var candidateDelay = cut - pos
            if (candidateDelay < MIN_CUT_LEAD_SECONDS) {
                candidateDelay += kotlin.math.ceil(
                    (MIN_CUT_LEAD_SECONDS - candidateDelay) / duration
                ) * duration
            }
            if (candidateDelay < delay) {
                delay = candidateDelay
                firstIndex = index
            }
        }
        if (firstIndex < 0 || !delay.isFinite()) return null

        val orderedCuts = List(sortedCuts.size) { offset ->
            sortedCuts[(firstIndex + offset) % sortedCuts.size]
        }
        val cut = orderedCuts.first()
        val sourceTime = tempoTimeToSourceTime(metaBpm, fromBpm, cut)
        val targetTime = sourceTimeToTempoTime(metaBpm, toBpm, sourceTime)
        val targetDuration = logicalDuration(name, toBpm)
        if (targetDuration <= 0.0) return null
        val targetOffsets = DoubleArray(orderedCuts.size) { index ->
            val source = tempoTimeToSourceTime(metaBpm, fromBpm, orderedCuts[index])
            val target = sourceTimeToTempoTime(metaBpm, toBpm, source)
            ((target - targetTime) % targetDuration + targetDuration) % targetDuration
        }
        val nextDelays = DoubleArray(orderedCuts.size) { index ->
            val next = orderedCuts[(index + 1) % orderedCuts.size]
            val current = orderedCuts[index]
            var interval = next - current
            if (interval <= 1e-6) interval += duration
            interval
        }
        return CutSelection(
            cutAtCurrentTempo = cut,
            delaySeconds = delay,
            sourceTime = sourceTime,
            targetTime = targetTime,
            targetDuration = targetDuration,
            targetOffsetsSec = targetOffsets,
            nextDelaysSec = nextDelays
        )
    }

    // ================= SBSMS 高质量预渲染（独立优先级队列） =================

    private fun startStretchPass(priority: String?, bpm: Double = getCurrentBpm()) {
        scheduleHighQualityPlan(resetStretchPass(priority, bpm))
    }

    /** 新速度建立独立 generation，并把四级高质量状态全部重置为0。 */
    private fun resetStretchPass(priority: String?, bpm: Double): StretchPass {
        val gen = stretchGeneration.incrementAndGet()
        currentBpm = bpm
        val ratio = stretchRatio(bpm, metaBpm)
        Log.i(TAG, "stretch pass gen=$gen bpm=$bpm loopBpm=$metaBpm ratio=$ratio priority=$priority")

        hqPlanRevision.incrementAndGet()
        nativeRenderCancelAll()
        releaseAllHqSessions()

        val keys = synchronized(opusData) { opusData.keys.toList() }
        val ordered = keys.sortedWith(compareBy({ indexIntOf(it) }, { categoryRankOf(it) }, { it }))
        val order = if (priority != null && ordered.contains(priority))
            listOf(priority) + ordered.filter { it != priority } else ordered
        val pass = StretchPass(gen, ratio, order, priority, bpm)
        activeStretchPass = pass

        synchronized(stateLock) {
            totalInPass = ordered.size
            readyCount = 0
            doneCount = 0
            states.clear()
            sgsmReadyCallbacks.clear()
            sgsmPlayableCallbacks.clear()
        }
        pushJs("onDrumLoopStretchReset($totalInPass)")
        return pass
    }

    /**
     * 严格按以下顺序执行：当前头部→当前完整→特殊一次性 variation 头部/完整→
     * 目标头部→目标完整→其他全部头部→其他全部末小节→其他全部完整。
     *
     * 暂时离开优先级前列的会话只关闭 MediaCodec，并保留 native SBSMS 状态和
     * 原始音频续算游标；再次轮到它时从上次喂入位置继续，不重新覆盖已增长的缓冲。
     */
    private fun scheduleHighQualityPlan(pass: StretchPass? = activeStretchPass) {
        val plan = pass ?: return
        if (plan.gen != stretchGeneration.get()) return
        val revision = hqPlanRevision.incrementAndGet()
        hqExec.execute {
            safeTask("hq-plan-${plan.gen}-$revision") { runHighQualityPlan(plan, revision) }
        }
    }

    private fun runHighQualityPlan(pass: StretchPass, revision: Int) {
        if (!isHqTaskCurrent(pass, revision)) return
        val current = playingVariation?.takeIf { pass.order.contains(it) }
        val special = oneShotVariation?.takeIf { pass.order.contains(it) }
        val target = targetVariation?.takeIf { pass.order.contains(it) }
        val staleSessions = synchronized(stateLock) {
            hqSessions.keys.filter { it != current && it != special && it != target }
        }
        staleSessions.forEach { parkHqSession(it) }

        if (current != null) {
            if (!ensureHqHead(current, pass, revision)) return
            if (!ensureHqFull(current, pass, revision)) return
        }
        if (special != null && special != current) {
            if (!ensureHqHead(special, pass, revision)) return
            if (!ensureHqFull(special, pass, revision)) return
        }
        if (target != null && target != current && target != special) {
            if (!ensureHqHead(target, pass, revision)) return
            if (!ensureHqFull(target, pass, revision)) return
        }

        val others = pass.order.filter { it != current && it != special && it != target }
        for (name in others) {
            if (!ensureHqHead(name, pass, revision)) return
            if ((synchronized(stateLock) { states[name] ?: STATE_NONE }) < STATE_DONE) {
                parkHqSession(name)
            }
        }
        for (name in others) {
            if (!ensureHqTail(name, pass, revision)) return
            if ((synchronized(stateLock) { states[name] ?: STATE_NONE }) < STATE_DONE) {
                parkHqSession(name)
            }
        }
        for (name in others) if (!ensureHqFull(name, pass, revision)) return
    }

    private fun isHqTaskCurrent(pass: StretchPass, revision: Int): Boolean =
        pass.gen == stretchGeneration.get() && revision == hqPlanRevision.get()

    private fun ensureHqHead(name: String, pass: StretchPass, revision: Int): Boolean {
        if ((synchronized(stateLock) { states[name] ?: STATE_NONE }) >= STATE_PRE_ROLLED) return true
        val session = getOrStartHqSession(
            name, pass, HqLayout.HEAD, sourceStartSec = 0.0, logicalOriginSec = 0.0
        ) ?: return false
        val targetFrames = (session.sampleRate * PRE_ROLL_SECONDS).toLong()
        if (!feedHqTo(name, session, pass, revision, targetFrames)) return false
        if (session.inputEnded) {
            if (!drainHq(name, pass, revision)) return false
            if (!commitHqState(name, pass, STATE_DONE)) return false
            removeHqSession(name)
        } else if (!commitHqState(name, pass, STATE_PRE_ROLLED)) {
            return false
        }
        return isHqTaskCurrent(pass, revision)
    }

    private fun ensureHqTail(name: String, pass: StretchPass, revision: Int): Boolean {
        if ((synchronized(stateLock) { states[name] ?: STATE_NONE }) >= STATE_TAIL_READY) return true
        val duration = logicalDuration(name, pass.bpm)
        if (duration <= 0.0) return false
        val measureSeconds = (beatsPerBar.coerceAtLeast(1) * 60.0 / pass.bpm)
            .coerceAtMost(duration)
        val tailStart = (duration - measureSeconds).coerceAtLeast(0.0)
        val sourceStart = tempoTimeToSourceTime(metaBpm, pass.bpm, tailStart)
        val session = getOrStartHqSession(
            name, pass, HqLayout.ROTATED_TAIL, sourceStart, tailStart
        ) ?: return false
        // 旋转布局先得到最后一小节，再得到头0.5秒；发布状态2时两段均已就绪。
        val targetSeconds = (measureSeconds + PRE_ROLL_SECONDS).coerceAtMost(duration)
        val targetFrames = (session.sampleRate * targetSeconds).toLong()
        if (!feedHqTo(name, session, pass, revision, targetFrames)) return false
        if (session.inputEnded) {
            if (!drainHq(name, pass, revision)) return false
            if (!commitHqState(name, pass, STATE_DONE)) return false
            removeHqSession(name)
        } else if (!commitHqState(name, pass, STATE_TAIL_READY)) {
            return false
        }
        return isHqTaskCurrent(pass, revision)
    }

    private fun ensureHqFull(name: String, pass: StretchPass, revision: Int): Boolean {
        val state = synchronized(stateLock) { states[name] ?: STATE_NONE }
        if (state >= STATE_DONE) return true

        val session = if (state >= STATE_TAIL_READY) {
            val duration = logicalDuration(name, pass.bpm)
            val measureSeconds = (beatsPerBar.coerceAtLeast(1) * 60.0 / pass.bpm)
                .coerceAtMost(duration)
            val tailStart = (duration - measureSeconds).coerceAtLeast(0.0)
            getOrStartHqSession(
                name, pass, HqLayout.ROTATED_TAIL,
                tempoTimeToSourceTime(metaBpm, pass.bpm, tailStart), tailStart
            )
        } else {
            getOrStartHqSession(name, pass, HqLayout.HEAD, 0.0, 0.0)
        } ?: return false

        // MediaCodec 可以在阶段之间停放，但 native SBSMS 流不会重建；恢复后从记录的
        // 原始音频游标继续。因此 state1 的下一段是头部之后，state2 的下一段是中间部分。
        if (state == STATE_PRE_ROLLED && session.layout == HqLayout.HEAD &&
            session.progressFrames < (session.sampleRate * PRE_ROLL_SECONDS).toLong()
        ) {
            val headFrames = (session.sampleRate * PRE_ROLL_SECONDS).toLong()
            if (!feedHqTo(name, session, pass, revision, headFrames)) return false
            if (!session.inputEnded && !commitHqState(name, pass, STATE_PRE_ROLLED)) return false
        }
        if (state == STATE_TAIL_READY && session.layout == HqLayout.ROTATED_TAIL) {
            val duration = logicalDuration(name, pass.bpm)
            val measureSeconds = (beatsPerBar.coerceAtLeast(1) * 60.0 / pass.bpm)
                .coerceAtMost(duration)
            val checkpointFrames = (session.sampleRate *
                (measureSeconds + PRE_ROLL_SECONDS).coerceAtMost(duration)).toLong()
            if (session.progressFrames < checkpointFrames) {
                if (!feedHqTo(name, session, pass, revision, checkpointFrames)) return false
                if (!session.inputEnded &&
                    !commitHqState(name, pass, STATE_TAIL_READY)
                ) return false
            }
        }
        if (!feedHqTo(name, session, pass, revision, Long.MAX_VALUE)) return false
        if (!session.inputEnded || !drainHq(name, pass, revision)) return false
        if (!commitHqState(name, pass, STATE_DONE)) return false
        removeHqSession(name)
        return isHqTaskCurrent(pass, revision)
    }

    private fun getOrStartHqSession(
        name: String,
        pass: StretchPass,
        layout: HqLayout,
        sourceStartSec: Double,
        logicalOriginSec: Double
    ): HqSession? {
        val existing = synchronized(stateLock) {
            hqSessions[name]?.takeIf { it.gen == pass.gen && it.layout == layout }
        }
        if (existing != null) {
            if (!existing.inputEnded && existing.decoder == null) {
                val resumed = createHqResumeDecoder(name, existing) ?: return null
                if (pass.gen != stretchGeneration.get()) {
                    resumed.release()
                    return null
                }
                val accepted = synchronized(stateLock) {
                    if (pass.gen == stretchGeneration.get() &&
                        hqSessions[name] === existing && existing.decoder == null
                    ) {
                        existing.decoder = resumed
                        true
                    } else false
                }
                if (!accepted) {
                    resumed.release()
                    return null
                }
                Log.d(TAG, "resume HQ $name layout=${existing.layout} " +
                    "sourceUs=${existing.resumeSourceUs} wrapped=${existing.resumeWrapped}")
            }
            return existing
        }
        removeHqSession(name)
        val decoder = createStreamingDecoder(name, sourceStartSec) ?: return null
        if (pass.gen != stretchGeneration.get()) {
            decoder.release()
            return null
        }
        val started = nativeRenderStart(
            name, decoder.sampleRate, pass.ratio, pass.gen,
            logicalOriginSec, logicalDuration(name, pass.bpm)
        )
        if (!started) {
            decoder.release()
            return null
        }
        if (pass.gen != stretchGeneration.get()) {
            decoder.release()
            return null
        }
        val rotationSplitUs = if (layout == HqLayout.ROTATED_TAIL) {
            (sourceStartSec.coerceAtLeast(0.0) * 1_000_000.0).toLong()
        } else 0L
        val session = HqSession(
            decoder = decoder,
            gen = pass.gen,
            layout = layout,
            logicalOriginSec = logicalOriginSec,
            sampleRate = decoder.sampleRate,
            rotationSplitUs = rotationSplitUs,
            resumeSourceUs = decoder.sourceCursorUs,
            resumeWrapped = decoder.hasWrapped
        )
        val accepted = synchronized(stateLock) {
            if (pass.gen == stretchGeneration.get()) {
                hqSessions[name] = session
                true
            } else false
        }
        if (!accepted) {
            decoder.release()
            return null
        }
        return session
    }

    /** feed 到指定输出帧数；Long.MAX_VALUE 表示一直送到输入结束。 */
    private fun feedHqTo(
        name: String,
        session: HqSession,
        pass: StretchPass,
        revision: Int,
        targetFrames: Long
    ): Boolean {
        while (!session.inputEnded && session.progressFrames < targetFrames) {
            if (!isHqTaskCurrent(pass, revision)) return false
            val decoder = session.decoder ?: return false
            val chunk = decoder.nextChunk(CHUNK_FRAMES)
            session.resumeSourceUs = decoder.sourceCursorUs
            session.resumeWrapped = decoder.hasWrapped
            if (chunk == null) {
                session.inputEnded = true
                break
            }
            val progress = nativeRenderFeed(name, chunk, pass.gen)
            if (progress < 0) return false
            session.progressFrames = progress
        }
        return isHqTaskCurrent(pass, revision)
    }

    private fun drainHq(name: String, pass: StretchPass, revision: Int): Boolean {
        var guard = 0
        while (isHqTaskCurrent(pass, revision) && guard++ < 20000) {
            val n = nativeRenderFinishStep(name, pass.gen)
            if (n == 0L) return true
            if (n < 0L) return false
        }
        return false
    }

    private fun commitHqState(name: String, pass: StretchPass, newState: Int): Boolean {
        if (!nativeRenderCommit(name, pass.gen, newState)) return false
        var oldState: Int
        var ready: Int
        var done: Int
        var readyCallbacks: List<() -> Unit> = emptyList()
        var playableCallbacks: List<() -> Unit> = emptyList()
        synchronized(stateLock) {
            oldState = states[name] ?: STATE_NONE
            states[name] = maxOf(oldState, newState)
            readyCount = pass.order.count { (states[it] ?: STATE_NONE) >= STATE_PRE_ROLLED }
            doneCount = pass.order.count { (states[it] ?: STATE_NONE) >= STATE_DONE }
            ready = readyCount
            done = doneCount
            if (newState >= STATE_DONE) {
                val key = sgsmKey(name, pass.gen)
                readyCallbacks = sgsmReadyCallbacks.remove(key).orEmpty()
                playableCallbacks = sgsmPlayableCallbacks.remove(key).orEmpty()
            }
        }
        if (oldState < STATE_PRE_ROLLED && newState >= STATE_PRE_ROLLED) {
            pushJs("onDrumLoopPreRoll('$name',$ready,$totalInPass)")
        }
        if (oldState < STATE_TAIL_READY && newState >= STATE_TAIL_READY) {
            pushJs("if(typeof onDrumLoopTailReady==='function')onDrumLoopTailReady('$name')")
        }
        if (oldState < STATE_DONE && newState >= STATE_DONE) {
            pushJs("onDrumLoopStretched('$name',$done,$totalInPass)")
            if (done >= totalInPass) pushJs("onDrumLoopStretchDone()")
        }
        readyCallbacks.forEach { callback -> runCatching(callback).onFailure {
            Log.e(TAG, "ready callback failed: $name", it)
        } }
        playableCallbacks.forEach { callback -> runCatching(callback).onFailure {
            Log.e(TAG, "playable callback failed: $name", it)
        } }
        return true
    }

    private fun sgsmKey(name: String, gen: Int): String = "$gen\u0000$name"

    private fun isSgsmTaskLive(key: String, token: Int): Boolean =
        synchronized(stateLock) {
            liveSgsmKeys.contains(key) && sgsmClaims[key] == token
        }

    private fun retainActiveSgsm(name: String, gen: Int) {
        activePlaybackGeneration = gen
        val key = sgsmKey(name, gen)
        synchronized(stateLock) {
            liveSgsmKeys.retainAll(setOf(key))
            liveSgsmKeys.add(key)
        }
    }

    private fun retainCurrentAndTargetSgsm(current: String, target: String, targetGen: Int) {
        val currentGen = activePlaybackGeneration.takeIf { it >= 0 } ?: targetGen
        val keep = setOf(sgsmKey(current, currentGen), sgsmKey(target, targetGen))
        synchronized(stateLock) { liveSgsmKeys.retainAll(keep) }
    }

    private fun retainOneShotSgsm(special: String, next: String?, gen: Int) {
        val keep = buildSet {
            add(sgsmKey(special, gen))
            if (!next.isNullOrBlank()) add(sgsmKey(next, gen))
        }
        synchronized(stateLock) {
            liveSgsmKeys.retainAll(keep)
            liveSgsmKeys.addAll(keep)
        }
        activePlaybackGeneration = gen
    }

    private fun drainSgsmToComplete(name: String, gen: Int, key: String, token: Int): Boolean {
        var guard = 0
        while (isSgsmTaskLive(key, token) && guard++ < 20000) {
            val n = nativeSgsmFinishStep(name, gen)
            if (n == 0L) return true
            if (n < 0) return false
        }
        return false
    }

    // ================= Signalsmith 按需渲染 =================

    /** 低质量低延迟渲染（状态0点击或 cut-point 变速）：流式、几乎立即出声。 */
    private fun submitSgsmOnDemand(
        name: String,
        sourceStartSec: Double = 0.0,
        gen: Int = stretchGeneration.get()
    ) {
        val key = sgsmKey(name, gen)
        val token: Int
        synchronized(stateLock) {
            if (sgsmDone.contains(key) || sgsmClaims.containsKey(key)) return
            token = sgsmTokenCounter.incrementAndGet()
            sgsmClaims[key] = token
            liveSgsmKeys.add(key)
        }
        onDemandExec.execute {
            safeTask(name) { taskOnDemandSgsm(name, gen, key, token, sourceStartSec) }
        }
    }

    private fun taskOnDemandSgsm(
        name: String,
        gen: Int,
        key: String,
        token: Int,
        sourceStartSec: Double
    ) {
        var dec: PcmChunkSource? = null
        var readyCallbacks: List<() -> Unit> = emptyList()
        var playableCallbacks: List<() -> Unit> = emptyList()
        var completed = false
        var preparationFailed = false
        try {
            if (!isSgsmTaskLive(key, token)) return
            // Signalsmith 与 SBSMS 可能同时准备同一 variation，不能共享一个 MediaCodec 会话。
            dec = createStreamingDecoder(name, sourceStartSec) ?: return
            while (isSgsmTaskLive(key, token)) {
                val chunk = dec.nextChunk(CHUNK_FRAMES) ?: break
                val progress = nativeSgsmFeed(name, chunk, gen)
                if (progress < 0) return
                if (progress >= (metaSampleRate * SPECIAL_START_WATERMARK_SECONDS).toLong()) {
                    playableCallbacks = synchronized(stateLock) {
                        sgsmPlayableCallbacks.remove(key).orEmpty()
                    }
                    playableCallbacks.forEach { callback -> runCatching(callback).onFailure {
                        Log.e(TAG, "Signalsmith playable callback failed: $name", it)
                    } }
                    playableCallbacks = emptyList()
                }
            }
            if (!isSgsmTaskLive(key, token)) return
            if (!drainSgsmToComplete(name, gen, key, token)) {
                pushJs("onDrumLoopError('渲染失败: $name')")
                return
            }
            synchronized(stateLock) {
                sgsmDone.add(key)
                readyCallbacks = sgsmReadyCallbacks.remove(key).orEmpty()
                playableCallbacks = sgsmPlayableCallbacks.remove(key).orEmpty()
            }
            completed = true
        } finally {
            synchronized(stateLock) {
                if (sgsmClaims[key] == token) sgsmClaims.remove(key)
                if (!completed) {
                    preparationFailed = !sgsmReadyCallbacks.remove(key).isNullOrEmpty() ||
                        !sgsmPlayableCallbacks.remove(key).isNullOrEmpty()
                    startedSgsm.remove(key)
                    sgsmLogicalOrigins.remove(key)
                }
            }
            dec?.release()
            if (preparationFailed) {
                val requestToken = oneShotSerial.get()
                val kind = oneShotKind
                if (kind != null &&
                    (oneShotVariation == name || resolveOneShotNext(kind) == name)
                ) {
                    failOneShotLaunch(requestToken, "特殊段落渲染失败: $name")
                }
            }
        }
        playableCallbacks.forEach { callback -> runCatching(callback).onFailure {
            Log.e(TAG, "Signalsmith playable callback failed after drain: $name", it)
        } }
        readyCallbacks.forEach { callback -> runCatching(callback).onFailure {
            Log.e(TAG, "Signalsmith ready callback failed: $name", it)
        } }
    }

    /** 在 variation 可从任意时间点安全读取后，以事件回调继续特殊播放流程。 */
    private fun whenFullySeekable(name: String, action: () -> Unit) {
        val gen = stretchGeneration.get()
        val key = sgsmKey(name, gen)
        val ready = synchronized(stateLock) {
            val isReady = (states[name] ?: STATE_NONE) >= STATE_DONE || sgsmDone.contains(key)
            if (!isReady) sgsmReadyCallbacks.getOrPut(key) { mutableListOf() }.add(action)
            isReady
        }
        if (ready) {
            action()
            return
        }

        if (!ensureSgsmStarted(name, gen = gen)) {
            synchronized(stateLock) { sgsmReadyCallbacks[key]?.remove(action) }
            pushJs("onDrumLoopError('无法准备特殊段落: ${jsEscape(name)}')")
            return
        }
        submitSgsmOnDemand(name, gen = gen)
    }

    /**
     * 当给定逻辑时间点已有足够的可播放数据时立即回调。HQ state1 可从 0 起播，
     * state2 还可从最后一小节起播，state3 可从任意位置起播。如果 HQ 尚不满足，
     * 从该时间点建立旋转 Signalsmith 流，一达到 250 ms 水位就回调，后续边播边渲染。
     */
    private fun whenPlayableAt(name: String, logicalStartSec: Double, action: () -> Unit) {
        val gen = stretchGeneration.get()
        val key = sgsmKey(name, gen)
        val duration = logicalDuration(name, currentBpm)
        if (duration <= 0.0) {
            pushJs("onDrumLoopError('无法读取特殊段落时长: ${jsEscape(name)}')")
            return
        }
        val start = ((logicalStartSec % duration) + duration) % duration
        val measure = currentMeasureSeconds().coerceAtMost(duration)
        val hqReady = synchronized(stateLock) {
            val state = states[name] ?: STATE_NONE
            state >= STATE_DONE ||
                (state >= STATE_PRE_ROLLED && start < 1e-3) ||
                (state >= STATE_TAIL_READY && start + 1e-3 >= duration - measure)
        }
        if (hqReady || synchronized(stateLock) { sgsmDone.contains(key) }) {
            action()
            return
        }

        var waitForFull = false
        var shouldStart = false
        synchronized(stateLock) {
            val claimActive = sgsmClaims.containsKey(key)
            val knownOrigin = sgsmLogicalOrigins[key]
            if (claimActive && knownOrigin != null &&
                kotlin.math.abs(knownOrigin - start) <= 1e-3
            ) {
                sgsmPlayableCallbacks.getOrPut(key) { mutableListOf() }.add(action)
                return@synchronized
            }
            if (claimActive || (startedSgsm.contains(key) && knownOrigin != null)) {
                // 已有另一个起点的流正在产生；完整后同一旋转缓冲可任意定位。
                sgsmReadyCallbacks.getOrPut(key) { mutableListOf() }.add(action)
                waitForFull = true
                return@synchronized
            }
            startedSgsm.remove(key)
            sgsmPlayableCallbacks.getOrPut(key) { mutableListOf() }.add(action)
            shouldStart = true
        }
        if (waitForFull) return
        if (!shouldStart) return

        val sourceStart = tempoTimeToSourceTime(metaBpm, currentBpm, start)
        if (!ensureSgsmStarted(name, start, duration, gen)) {
            synchronized(stateLock) { sgsmPlayableCallbacks[key]?.remove(action) }
            pushJs("onDrumLoopError('无法准备特殊段落: ${jsEscape(name)}')")
            return
        }
        submitSgsmOnDemand(name, sourceStart, gen)
    }

    // ================= 播放控制 (播放状态机) =================

    @Volatile private var targetVariation: String? = null

    /**
     * 若当前是四小节主段落、HQ 尾段已达到 state2，且本小节剩余时间内能找到
     * 一对低 RMS 切点，则让 native 在最早可用点把当前音频等功率交叉淡化到最后一小节。
     * 返回 false 时仍保留普通的“下一小节第一拍切目标”行为。
     */
    private fun tryArmAutoFill(current: String): Boolean {
        if (categoryOf(current) !in setOf("verse", "verse2", "prechorus", "chorus")) {
            return false
        }
        val snapshot = synchronized(stateLock) {
            val index = variationIndex[current] ?: return false
            Triple(
                states[current] ?: STATE_NONE,
                currentCutPoints.getOrNull(index)?.toList().orEmpty(),
                currentDurations.getOrNull(index) ?: 0.0
            )
        }
        if (snapshot.first < STATE_TAIL_READY) return false

        val bpm = activePlaybackBpm.takeIf { it > 0.0 } ?: currentBpm
        if (bpm <= 0.0) return false
        val measureSec = beatsPerBar.coerceAtLeast(1) * 60.0 / bpm
        val durationSec = snapshot.third
        val bars = kotlin.math.round(durationSec / measureSec).toInt()
        // 自动加花只使用已确认含末小节加花的四小节主 variation。
        if (bars != 4 || kotlin.math.abs(durationSec - measureSec * 4.0) > measureSec * 0.05) {
            return false
        }

        val position = nativeGetPlaybackPosition(current)
        // 以读取原生播放头的时刻为倒计时基准；Kotlin 计算/桥接消耗的时间必须扣除，
        // 否则 A 切点会被系统性地晚触发。
        val positionReadNs = System.nanoTime()
        if (position < 0.0) return false
        val transition = findAutoFillTransition(
            snapshot.second, position, durationSec, measureSec
        ) ?: run {
            Log.d(TAG, "auto fill skipped: no cut pair before bar end name=$current pos=$position")
            return false
        }

        val deadlineNs = positionReadNs +
            (transition.delaySeconds * 1_000_000_000.0).toLong()
        val remainingNs = deadlineNs - System.nanoTime()
        // 已经明显错过 A 切点时不在错误位置强行跳转，退回普通 beat-0 切换。
        if (remainingNs < -2_000_000L) return false
        val remainingDelay = remainingNs.coerceAtLeast(0L) / 1_000_000_000.0
        val gen = stretchGeneration.get()
        val armed = nativeArmAutoFillJump(
            current, gen, remainingDelay, transition.destinationTime
        )
        if (armed) {
            Log.i(TAG, "auto fill armed: $current pos=$position " +
                "a=${transition.sourceTime} b=${transition.destinationTime} " +
                "delay=$remainingDelay state=${snapshot.first} gen=$gen")
        } else {
            Log.w(TAG, "auto fill rejected by native: $current state=${snapshot.first} gen=$gen")
        }
        return armed
    }

    private fun exactVariation(name: String): String? = synchronized(opusData) {
        name.takeIf { opusData.containsKey(it) }
    }

    private fun currentMeasurePhase(reference: String): Double? {
        val position = nativeGetPlaybackPosition(reference)
        val measure = currentMeasureSeconds()
        if (position < 0.0 || measure <= 0.0) return null
        return ((position % measure) + measure) % measure
    }

    private fun oneShotFadeMs(kind: OneShotKind): Int = when (kind) {
        OneShotKind.SMART_FILL -> SMART_FILL_FADE_MS
        OneShotKind.INTRO -> INTRO_FADE_MS
        OneShotKind.BREAK -> BREAK_FADE_MS
        OneShotKind.BREAK_TARGET -> SMART_FILL_FADE_MS
        OneShotKind.ENDING -> ENDING_FADE_MS
    }

    private fun resolveOneShotNext(kind: OneShotKind): String? = when (kind) {
        OneShotKind.SMART_FILL, OneShotKind.INTRO,
        OneShotKind.BREAK, OneShotKind.BREAK_TARGET -> targetVariation
        OneShotKind.ENDING -> null
    }

    private fun isOneShotRequestCurrent(token: Int, kind: OneShotKind, name: String): Boolean =
        oneShotSerial.get() == token && oneShotKind == kind && oneShotVariation == name

    private fun clearOneShotState(cancelNative: Boolean, clearIntro: Boolean) {
        oneShotSerial.incrementAndGet()
        synchronized(oneShotLock) {
            oneShotKind = null
            oneShotVariation = null
            oneShotStarted = false
            oneShotAudioStarted = false
            oneShotFixedReturn = null
            oneShotPhaseReference = null
            oneShotStartInLastMeasure = false
            oneShotStartDeadlineNs = 0L
            if (clearIntro) introSelected = false
        }
        if (cancelNative) nativeCancelOneShot()
    }

    private fun oneShotLogicalStart(name: String, fallback: Double): Double {
        val phase = oneShotPhaseReference?.let { currentMeasurePhase(it) } ?: fallback
        if (!oneShotStartInLastMeasure) return phase
        val duration = logicalDuration(name, currentBpm)
        if (duration <= 0.0) return phase
        val measure = currentMeasureSeconds().coerceAtMost(duration)
        if (measure <= 0.0) return phase
        val normalizedPhase = ((phase % measure) + measure) % measure
        return ((duration - measure).coerceAtLeast(0.0) + normalizedPhase)
            .coerceAtMost((duration - 1e-6).coerceAtLeast(0.0))
    }

    /**
     * ENDING 的边界截止时间在点击瞬间确定；若准备工作错过了该边界，则顺延到
     * 下一个完整小节边界，绝不在小节中途启动。
     */
    private fun remainingOneShotStartDelaySec(): Double {
        var deadline = oneShotStartDeadlineNs
        if (deadline <= 0L) return 0.0
        val now = System.nanoTime()
        if (deadline <= now) {
            val measureNs = (currentMeasureSeconds() * 1_000_000_000.0).toLong()
                .coerceAtLeast(1L)
            val periods = (now - deadline) / measureNs + 1L
            deadline += periods * measureNs
            oneShotStartDeadlineNs = deadline
        }
        return (deadline - now).coerceAtLeast(0L) / 1_000_000_000.0
    }

    private fun nextMeasureBoundaryDeadlineNs(reference: String): Long {
        val position = nativeGetPlaybackPosition(reference)
        val readAt = System.nanoTime()
        val measure = currentMeasureSeconds()
        if (measure <= 0.0) return 0L
        val phase = if (position >= 0.0) {
            ((position % measure) + measure) % measure
        } else {
            0.0
        }
        var remaining = measure - phase
        // 点击恰好落在第一拍也表示“播完当前小节”，因此仍等待一个完整小节。
        if (remaining < 0.001) remaining = measure
        return readAt + (remaining * 1_000_000_000.0).toLong()
    }

    /**
     * 特殊段落准备完成后开始播放。目标在准备期间可变，所以每次真正启动前都重新读取。
     */
    private fun prepareOneShotLaunch(
        token: Int,
        kind: OneShotKind,
        name: String,
        initialStart: Double,
        requireFullSource: Boolean = false
    ) {
        if (!isOneShotRequestCurrent(token, kind, name)) return
        val next = resolveOneShotNext(kind)
        if (kind != OneShotKind.ENDING && next.isNullOrBlank()) {
            failOneShotLaunch(token, "请先选择播放目标")
            return
        }

        val sourceReady = sourceReady@{
            if (!isOneShotRequestCurrent(token, kind, name)) return@sourceReady
            attemptOneShotLaunch(token, kind, name, next, initialStart, requireFullSource)
        }
        val prepareSource = {
            if (requireFullSource) whenFullySeekable(name, sourceReady)
            else whenPlayableAt(name, initialStart, sourceReady)
        }

        // Smart Fill/BREAK 的初始返回目标正在播放，已天然可用；其他目标先准备起播水位。
        val phaseReference = oneShotPhaseReference
        if (next.isNullOrBlank() || (!oneShotStarted && next == phaseReference)) {
            prepareSource()
        } else {
            whenPlayableAt(next, 0.0, prepareSource)
        }
    }

    private fun attemptOneShotLaunch(
        token: Int,
        kind: OneShotKind,
        name: String,
        preparedNext: String?,
        initialStart: Double,
        fallbackUsed: Boolean
    ) {
        if (!isOneShotRequestCurrent(token, kind, name)) return
        val latestNext = resolveOneShotNext(kind)
        if (latestNext != preparedNext) {
            prepareOneShotLaunch(token, kind, name, initialStart, fallbackUsed)
            return
        }

        val start = oneShotLogicalStart(name, initialStart)
        val duration = logicalDuration(name, currentBpm)
        val safeStart = if (duration > 0.0) ((start % duration) + duration) % duration else start
        val stopAfter = kind == OneShotKind.ENDING
        val nextName = latestNext.orEmpty()
        val startDelay = remainingOneShotStartDelaySec()
        val endFadeSeconds = if (stopAfter) currentMeasureSeconds() * 0.125 else 0.0

        val started = synchronized(oneShotLock) {
            if (!isOneShotRequestCurrent(token, kind, name) || oneShotStarted) {
                false
            } else {
                val ok = nativeStartOneShot(
                    name, safeStart, oneShotFadeMs(kind), nextName, stopAfter,
                    startDelay, endFadeSeconds
                )
                if (ok) oneShotStarted = true
                ok
            }
        }
        if (!started) {
            if (!isOneShotRequestCurrent(token, kind, name) || oneShotStarted) return
            if (!fallbackUsed) {
                // 流式水位达标但最新相位仍超出已发布数据时，等完整旋转缓冲后再试一次。
                prepareOneShotLaunch(token, kind, name, initialStart, true)
            } else {
                failOneShotLaunch(token, "特殊段落缓冲未准备好: $name")
            }
            return
        }

        val gen = stretchGeneration.get()
        retainOneShotSgsm(name, latestNext, gen)
        scheduleHighQualityPlan()
        if (startDelay <= 1e-6) {
            markOneShotAudioStarted(kind, name)
            if (kind == OneShotKind.INTRO || kind == OneShotKind.ENDING) nativeSyncBeat()
            Log.i(TAG, "one-shot started kind=${kind.jsName} name=$name " +
                "start=$safeStart next=$nextName")
        } else {
            Log.i(TAG, "one-shot armed kind=${kind.jsName} name=$name start=$safeStart " +
                "delay=$startDelay next=$nextName")
        }
    }

    private fun markOneShotAudioStarted(kind: OneShotKind, name: String) {
        val shouldPublish = synchronized(oneShotLock) {
            if (oneShotKind != kind || oneShotVariation != name || !oneShotStarted ||
                oneShotAudioStarted
            ) {
                false
            } else {
                oneShotAudioStarted = true
                true
            }
        }
        if (!shouldPublish) return
        playingVariation = name
        scheduleHighQualityPlan()
        pushJs("if(typeof onDrumOneShotStarted==='function')" +
            "onDrumOneShotStarted('${kind.jsName}','${jsEscape(name)}')")
    }

    /** 延迟的一次性段落已在 native 音频回调的精确小节边界开始。 */
    fun onOneShotStartedFired(name: String) {
        val kind = oneShotKind ?: return
        if (oneShotVariation != name) return
        markOneShotAudioStarted(kind, name)
        Log.i(TAG, "delayed one-shot started kind=${kind.jsName} name=$name")
    }

    private fun failOneShotLaunch(token: Int, message: String) {
        if (oneShotSerial.get() != token) return
        clearOneShotState(cancelNative = true, clearIntro = false)
        scheduleHighQualityPlan()
        pushJs("if(typeof onDrumOneShotCancelled==='function')onDrumOneShotCancelled()")
        pushJs("onDrumLoopError('${jsEscape(message)}')")
    }

    private fun requestOneShot(
        kind: OneShotKind,
        name: String,
        phaseReference: String?,
        fixedReturn: String?,
        replaceExisting: Boolean = false,
        startInLastMeasure: Boolean = false,
        waitForMeasureEnd: Boolean = false
    ) {
        if (!folderLoaded || !synchronized(opusData) { opusData.containsKey(name) }) return
        if (oneShotKind != null && !replaceExisting) return
        if (synchronized(bpmTransitionLock) { bpmTransitionActive }) {
            pushJs("onDrumLoopPlaybackBlocked('请等待当前变速完成')")
            return
        }

        val boundaryReference = playingVariation
        val startDeadline = if (waitForMeasureEnd && boundaryReference != null) {
            nextMeasureBoundaryDeadlineNs(boundaryReference)
        } else {
            0L
        }
        if (replaceExisting) nativeCancelOneShot()

        val token = oneShotSerial.incrementAndGet()
        synchronized(oneShotLock) {
            oneShotKind = kind
            oneShotVariation = name
            oneShotStarted = false
            oneShotAudioStarted = false
            oneShotFixedReturn = fixedReturn
            oneShotPhaseReference = phaseReference
            oneShotStartInLastMeasure = startInLastMeasure
            oneShotStartDeadlineNs = startDeadline
        }
        nativeCancelAutoFillJump()
        nativeRequestSwitch("")

        if (kind == OneShotKind.SMART_FILL || kind == OneShotKind.BREAK ||
            kind == OneShotKind.BREAK_TARGET
        ) {
            targetVariation = fixedReturn ?: phaseReference
            targetVariation?.let { pushJs("onDrumLoopPending('${jsEscape(it)}')") }
        }
        scheduleHighQualityPlan()
        pushJs("if(typeof onDrumOneShotPreparing==='function')" +
            "onDrumOneShotPreparing('${kind.jsName}','${jsEscape(name)}')")

        val initialStart = oneShotLogicalStart(name, 0.0)
        prepareOneShotLaunch(token, kind, name, initialStart)
    }

    /** 停止时 INTRO 可作为起始段落单独选中，下方表格仍保留其独立目标。 */
    fun setIntroSelected(selected: Boolean) {
        if (playingVariation != null || oneShotKind != null) return
        val intro = exactVariation("INTRO")
        introSelected = selected && intro != null
        oneShotVariation = intro.takeIf { introSelected }
        scheduleHighQualityPlan()
        pushJs("if(typeof onDrumIntroSelection==='function')onDrumIntroSelection($introSelected)")
    }

    /** 把当前可见主 variation 的 RMS 分位映射到 Fill/Ending 等候选组。 */
    private fun findRankMappedSpecial(current: String, candidateCategory: String): String? {
        val snapshot = synchronized(stateLock) {
            val mains = variationKeys.asSequence()
                .filter { categoryOf(it) in MAIN_VARIATION_CATEGORIES }
                .mapNotNull { key -> averageRmsByVariation[key]?.let { key to it } }
                .toList()
            val candidates = variationKeys.asSequence()
                .filter { categoryOf(it) == candidateCategory }
                .mapNotNull { key -> averageRmsByVariation[key]?.let { key to it } }
                .toList()
            mains to candidates
        }
        return findRankMappedVariation(current, snapshot.first, snapshot.second)
    }

    fun triggerSmartFill() {
        val current = playingVariation ?: return
        if (oneShotKind != null) return
        val fill = findRankMappedSpecial(current, "fill")
        if (fill == null) {
            pushJs("onDrumLoopPlaybackBlocked('当前 variation 或 FILL 缺少可用的 RMS 数据')")
            return
        }
        requestOneShot(OneShotKind.SMART_FILL, fill, current, current)
    }

    fun triggerBreak() {
        val current = playingVariation ?: return
        val name = exactVariation("BREAK") ?: run {
            pushJs("onDrumLoopPlaybackBlocked('当前鼓循环没有 BREAK')")
            return
        }
        requestOneShot(OneShotKind.BREAK, name, current, null)
    }

    fun triggerEnding() {
        val current = playingVariation ?: return
        if (oneShotKind != null) return
        val name = findRankMappedSpecial(current, "ending") ?: run {
            pushJs("onDrumLoopPlaybackBlocked('当前 variation 或 ENDING 缺少可用的 RMS 数据')")
            return
        }
        requestOneShot(
            OneShotKind.ENDING, name, null, null,
            waitForMeasureEnd = true
        )
    }

    /** BREAK 正在发声时，立即淡化到新目标的最后一小节，播完后从目标开头循环。 */
    private fun redirectBreakToTarget(variation: String) {
        val breakName = playingVariation ?: return
        requestOneShot(
            OneShotKind.BREAK_TARGET,
            variation,
            phaseReference = breakName,
            fixedReturn = variation,
            replaceExisting = true,
            startInLastMeasure = true
        )
    }

    /** 点击 variation 按钮: 设为目标 (白框闪烁); 播放中则请求第一拍切换。 */
    fun setPending(variation: String) {
        val special = oneShotKind
        if (special != null) {
            if ((special == OneShotKind.BREAK || special == OneShotKind.BREAK_TARGET) &&
                playingVariation == "BREAK"
            ) {
                redirectBreakToTarget(variation)
                return
            }
            if (special == OneShotKind.SMART_FILL || special == OneShotKind.INTRO ||
                special == OneShotKind.BREAK ||
                special == OneShotKind.BREAK_TARGET
            ) {
                targetVariation = variation
                scheduleHighQualityPlan()
                val token = oneShotSerial.get()
                if (oneShotStarted) {
                    whenPlayableAt(variation, 0.0) {
                        if (oneShotSerial.get() == token && oneShotKind == special &&
                            targetVariation == variation && nativeUpdateOneShotNext(variation)
                        ) {
                            retainOneShotSgsm(oneShotVariation.orEmpty(), variation, stretchGeneration.get())
                            Log.i(TAG, "one-shot target updated: ${special.jsName} -> $variation")
                        }
                    }
                }
                pushJs("onDrumLoopPending('${jsEscape(variation)}')")
            } else {
                // ENDING 始终播完停止，不接受后续 variation 目标。
                targetVariation?.let { pushJs("onDrumLoopPending('${jsEscape(it)}')") }
            }
            return
        }

        targetVariation = variation
        scheduleHighQualityPlan()
        val playing = playingVariation
        if (playing != null) {
            val tempoTransitionRunning = synchronized(bpmTransitionLock) { bpmTransitionActive }
            if (!tempoTransitionRunning) {
                // 新点击覆盖尚未发生的加花跳转；如果当前已经跳到最后一小节，位置检查
                // 会自然拒绝再次跳转，但下面的 beat-0 目标仍会更新。
                nativeCancelAutoFillJump()
                val autoFillArmed = tryArmAutoFill(playing)
                retainCurrentAndTargetSgsm(playing, variation, stretchGeneration.get())
                prepareForPlayback(variation)
                // 包括 variation == playing：成功加花后在下一小节第一拍从头重播自身；
                // 若条件不满足，则退化为普通的下一小节第一拍重播。
                nativeRequestSwitch(variation)
                Log.d(TAG, "variation requested: $playing -> $variation autoFill=$autoFillArmed")
            }
            // 变速锁内只更新目标；待当前/最新变速完成后再提交普通切换，
            // 避免 beat-0 play() 把正在等待 cut point 的播放槽换走。
        }
        pushJs("onDrumLoopPending('${jsEscape(variation)}')")
    }

    /** 播放按钮: 立即播放待播放的 variation, 并同步节拍器到第一拍 */
    fun startPlayback() {
        val p = targetVariation
        if (p == null || !folderLoaded || !synchronized(opusData) { opusData.containsKey(p) }) {
            Log.w(TAG, "startPlayback ignored: no fully loaded pending variation")
            pushJs("onDrumLoopPlaybackBlocked('请先选择并等待鼓循环加载完成')")
            return
        }
        activePlaybackBpm = currentBpm.takeIf { it > 0.0 } ?: getCurrentBpm()
        recalculateCurrentCutPoints(activePlaybackBpm)
        if (introSelected) {
            val intro = exactVariation("INTRO")
            if (intro != null) {
                requestOneShot(OneShotKind.INTRO, intro, null, null)
                return
            }
            introSelected = false
        }

        nativeCancelAutoFillJump()
        nativeRequestSwitch("")   // 清掉可能残留的切换请求
        prepareForPlayback(p)
        nativePlayAt(p, 0.0)
        nativeSyncBeat()
        playingVariation = p
        retainActiveSgsm(p, stretchGeneration.get())
        scheduleHighQualityPlan()
        pushJs("onDrumLoopPlaying('${jsEscape(p)}')")
    }

    /** 停止按钮: 1 秒淡出停止 */
    fun stopPlayback() {
        playingVariation = null
        activePlaybackGeneration = -1
        synchronized(stateLock) { liveSgsmKeys.clear() }
        clearOneShotState(cancelNative = true, clearIntro = true)
        cancelBpmTransition()
        nativeCancelAutoFillJump()
        nativeRequestSwitch("")
        nativeFadeOut(1000)
        // 停止只改变播放优先级，不停止后台高质量渲染。
        scheduleHighQualityPlan()
        pushJs("onDrumLoopStopping()")
    }

    /** 立即停止 (切换模式/切换鼓循环集时) */
    fun stopPlaybackImmediate() {
        playingVariation = null
        activePlaybackGeneration = -1
        synchronized(stateLock) { liveSgsmKeys.clear() }
        clearOneShotState(cancelNative = true, clearIntro = true)
        cancelBpmTransition()
        nativeCancelAutoFillJump()
        nativeRequestSwitch("")
        nativeStop()
        scheduleHighQualityPlan()
    }

    /** beat-0 切换已触发 (MainActivity 轮询回调) */
    fun onSwitchFired(name: String) {
        playingVariation = name
        retainActiveSgsm(name, stretchGeneration.get())
        scheduleHighQualityPlan()
    }

    /** 一次性段落在 native 音频回调中精确播完，已无缝切到返回/目标 variation。 */
    fun onOneShotSwitchFired(name: String) {
        val kind = oneShotKind
        val pendingAfterSwitch = targetVariation
        val deferred = deferredOneShotBpm
        deferredOneShotBpm = -1.0
        clearOneShotState(cancelNative = false, clearIntro = true)
        playingVariation = name
        if (kind == OneShotKind.SMART_FILL || kind == OneShotKind.INTRO ||
            kind == OneShotKind.BREAK ||
            kind == OneShotKind.BREAK_TARGET
        ) {
            targetVariation = pendingAfterSwitch ?: name
        } else {
            targetVariation = name
        }
        retainActiveSgsm(name, stretchGeneration.get())
        scheduleHighQualityPlan()
        pushJs("if(typeof onDrumOneShotFinished==='function')" +
            "onDrumOneShotFinished('${jsEscape(name)}','${kind?.jsName ?: ""}')")
        // 极小窗口内：音频回调已切换，但 UI 轮询尚未领取事件时，用户可能又点了目标。
        // 这时把新目标当作普通 beat-0 切换，不丢掉这次输入。
        if ((kind == OneShotKind.SMART_FILL || kind == OneShotKind.INTRO ||
                kind == OneShotKind.BREAK ||
                kind == OneShotKind.BREAK_TARGET) &&
            pendingAfterSwitch != null && pendingAfterSwitch != name
        ) {
            prepareForPlayback(pendingAfterSwitch)
            nativeRequestSwitch(pendingAfterSwitch)
        }
        if (deferred > 0.0) onBpmChangePreparing(deferred)
    }

    /** 淡出或 ENDING 自然播完后统一清理 Kotlin 状态。 */
    fun onPlaybackStopped() {
        val deferred = deferredOneShotBpm
        deferredOneShotBpm = -1.0
        playingVariation = null
        activePlaybackGeneration = -1
        synchronized(stateLock) { liveSgsmKeys.clear() }
        clearOneShotState(cancelNative = false, clearIntro = true)
        scheduleHighQualityPlan()
        if (deferred > 0.0) onBpmChangePreparing(deferred)
    }

    fun isPlaybackActive(): Boolean = playingVariation != null || oneShotKind != null

    /**
     * 四击 BPM 的第四击后立即进行新速度换代，并提前启动首个实际会播放
     * variation 的 Signalsmith 低延迟渲染。这样等到下一拍前 100ms 执行 play 时，
     * 音频水位已经就绪，而不是到启播时才开始拉伸。
     */
    fun prepareTapTempoStart(requestedBpm: Double) {
        onBpmChangePreparing(requestedBpm)
        if (!folderLoaded || playingVariation != null || oneShotKind != null) return
        val first = if (introSelected) exactVariation("INTRO") else targetVariation
        if (first != null && synchronized(opusData) { opusData.containsKey(first) }) {
            prepareForPlayback(first)
        }
    }

    /** 准备目标 variation 的渲染 (确保播放开始时有数据) */
    private fun prepareForPlayback(variation: String) {
        val state = synchronized(stateLock) { states[variation] ?: STATE_NONE }
        if (state >= STATE_PRE_ROLLED) {
            // 状态1可从头用 SBSMS 流式播放；状态2还支持末小节任意点；状态3支持任意点。
            scheduleHighQualityPlan()
            return
        }

        // 状态0：Signalsmith 低延迟引擎立即流式渲染；SBSMS 仍在独立队列后台运行。
        val gen = stretchGeneration.get()
        val key = sgsmKey(variation, gen)
        val done = synchronized(stateLock) { sgsmDone.contains(key) }
        if (!done) {
            val inProgress = synchronized(stateLock) { sgsmClaims.containsKey(key) }
            if (!inProgress) {
                synchronized(stateLock) { startedSgsm.remove(key) }
                if (ensureSgsmStarted(variation, gen = gen)) {
                    submitSgsmOnDemand(variation, gen = gen)
                }
            }
        }
        scheduleHighQualityPlan()
    }

    /** 默认待播放 variation: VERSE→VERSE2→PRECHORUS→CHORUS 类别中第一个 (序号最小) */
    fun pickDefaultPending(keys: List<String>): String? {
        if (keys.isEmpty()) return null
        val rank = listOf("verse", "verse2", "prechorus", "chorus")
        for (cat in rank) {
            val best = keys.filter { categoryOf(it) == cat }.minByOrNull { indexIntOf(it) }
            if (best != null) return best
        }
        return keys.minByOrNull { indexIntOf(it) }
    }

    /** 写入最新目标 BPM；已有变速流程运行时只覆盖目标，不打断当前 cut-point 流程。 */
    fun onBpmChangePreparing(requestedBpm: Double) {
        if (!folderLoaded || requestedBpm <= 0.0) return
        if (oneShotKind != null) {
            // 一次性段落的结束位置是按当前速度样本精确计数的；中途换代会破坏结束点。
            // 保留最新用户目标，在 Fill/INTRO/BREAK 返回或 ENDING 停止后立即执行。
            deferredOneShotBpm = requestedBpm
            Log.i(TAG, "defer BPM $requestedBpm until one-shot completes")
            return
        }
        val p = playingVariation
        if (p == null) {
            activePlaybackBpm = requestedBpm
            recalculateCurrentCutPoints(requestedBpm)
            synchronized(bpmTransitionLock) {
                targetBpm = -1.0
                bpmTransitionActive = false
            }
            startStretchPass(null, requestedBpm)
            return
        }

        var shouldStart = false
        synchronized(bpmTransitionLock) {
            targetBpm = requestedBpm
            if (!bpmTransitionActive &&
                kotlin.math.abs(requestedBpm - activePlaybackBpm) >= 0.001
            ) {
                bpmTransitionActive = true
                shouldStart = true
            } else if (!bpmTransitionActive) {
                targetBpm = -1.0
            }
        }
        Log.i(TAG, "BPM target updated: target=$requestedBpm active=$activePlaybackBpm running=${!shouldStart}")
        if (shouldStart) beginNextBpmTransition()
    }

    /** 长按不再锁住鼓循环变速；每个 +/- 步进只更新 targetBpm。保留入口兼容旧桥接。 */
    fun setBpmInputHeld(held: Boolean) {
        Log.d(TAG, "BPM input hold=${if (held) "on" else "off"} (target-only mode)")
    }

    private fun beginNextBpmTransition() {
        val name = playingVariation ?: run {
            cancelBpmTransition()
            return
        }
        val requested = synchronized(bpmTransitionLock) { targetBpm }
        val fromBpm = activePlaybackBpm.takeIf { it > 0.0 } ?: getCurrentBpm()
        if (requested <= 0.0 || kotlin.math.abs(requested - fromBpm) < 0.001) {
            synchronized(bpmTransitionLock) {
                targetBpm = -1.0
                bpmTransitionActive = false
            }
            return
        }

        // 暂停尚未到拍头的普通 variation 切换；变速流程结束后按 targetVariation 重新提交。
        nativeRequestSwitch("")
        nativeCancelAutoFillJump()
        retainActiveSgsm(
            name, activePlaybackGeneration.takeIf { it >= 0 } ?: stretchGeneration.get()
        )
        // 加锁后建立新 generation，清空四级 HQ 状态；SBSMS 独立队列立即按新速度运行，
        // 但变速是否执行只取决于下方 Signalsmith cut-point 流。
        val pass = resetStretchPass(name, requested)
        scheduleHighQualityPlan(pass)
        val position = nativeGetPlaybackPosition(name)
        val cut = if (position >= 0.0) {
            selectNextTempoCut(name, position, fromBpm, requested)
        } else null
        if (cut == null) {
            Log.e(TAG, "No usable cut point for $name at position=$position")
            pushJs("onDrumLoopError('无法为 $name 找到可用切分点')")
            synchronized(bpmTransitionLock) {
                bpmTransitionActive = false
                targetBpm = -1.0
            }
            startStretchPass(name, fromBpm)
            return
        }
        val cutDeadlineNs = System.nanoTime() +
            (cut.delaySeconds * 1_000_000_000.0).toLong()

        synchronized(bpmTransitionLock) {
            bpmTransitionTarget = requested
            bpmTransitionGen = pass.gen
            bpmTransitionName = name
        }

        // 目标低延迟流的物理第 0 帧对应新速度时间轴 t2；解码顺序为 [t0, end) + [0, t0)。
        val sgsmStarted = ensureSgsmStarted(
            name, cut.targetTime, cut.targetDuration, pass.gen
        )
        if (sgsmStarted) submitSgsmOnDemand(name, cut.sourceTime, pass.gen)
        val remainingDelay = ((cutDeadlineNs - System.nanoTime()).coerceAtLeast(0L) /
            1_000_000_000.0)
        val ready = sgsmStarted && nativeArmTempoCut(
            name,
            pass.gen,
            remainingDelay,
            cut.targetOffsetsSec,
            cut.nextDelaysSec,
            requested
        )
        if (!ready) {
            Log.e(TAG, "Failed to arm tempo cut: $name gen=${pass.gen}")
            pushJs("onDrumLoopError('切分点变速准备失败: $name')")
            synchronized(bpmTransitionLock) {
                bpmTransitionActive = false
                targetBpm = -1.0
                bpmTransitionGen = -1
                bpmTransitionName = null
            }
            startStretchPass(name, fromBpm)
            return
        }

        Log.i(TAG, "tempo cut: $name pos=$position t1=${cut.cutAtCurrentTempo} " +
            "delay=$remainingDelay " +
            "t0=${cut.sourceTime} t2=${cut.targetTime} " +
            "bpm=$fromBpm->$requested gen=${pass.gen}")
    }

    /** native 音频回调已在 cut point 切到 Signalsmith；此刻立即解锁，不等待 SBSMS。 */
    fun onBpmCutApplied(appliedBpm: Double) {
        var restart = false
        var deferredVariation: String? = null
        var appliedGen = -1
        var nextTarget = -1.0
        synchronized(bpmTransitionLock) {
            if (!bpmTransitionActive ||
                kotlin.math.abs(appliedBpm - bpmTransitionTarget) >= 0.001
            ) return
            appliedGen = bpmTransitionGen
            bpmTransitionActive = false
            bpmTransitionGen = -1
            bpmTransitionName = null
        }
        if (appliedGen >= 0) playingVariation?.let { retainActiveSgsm(it, appliedGen) }
        activePlaybackBpm = appliedBpm
        recalculateCurrentCutPoints(appliedBpm)
        synchronized(bpmTransitionLock) {
            if (targetBpm > 0.0 && kotlin.math.abs(targetBpm - activePlaybackBpm) >= 0.001) {
                bpmTransitionActive = true
                restart = true
                nextTarget = targetBpm
            } else {
                targetBpm = -1.0
                deferredVariation = targetVariation?.takeIf { it != playingVariation }
            }
        }
        if (restart) {
            // native 会先公布刚实际生效的中间 BPM；若还有最新目标，立即把 UI 恢复为目标预览。
            pushJs("previewBpmTarget(${Math.round(nextTarget)})")
            beginNextBpmTransition()
        } else {
            deferredVariation?.let {
                prepareForPlayback(it)
                nativeRequestSwitch(it)
            }
        }
    }

    private fun cancelBpmTransition() {
        nativeCancelTempoCut()
        synchronized(bpmTransitionLock) {
            targetBpm = -1.0
            bpmTransitionActive = false
            bpmTransitionTarget = -1.0
            bpmTransitionGen = -1
            bpmTransitionName = null
        }
    }

    // ================= 启动辅助 =================

    /** 确保本批次已为该 variation 启动 Signalsmith 渲染流 (幂等) */
    private fun ensureSgsmStarted(
        name: String,
        logicalOriginSec: Double = 0.0,
        durationSec: Double = logicalDuration(name, currentBpm),
        gen: Int = stretchGeneration.get()
    ): Boolean {
        val key = sgsmKey(name, gen)
        synchronized(stateLock) {
            if (startedSgsm.contains(key)) return true
            startedSgsm.add(key)
            liveSgsmKeys.add(key)
            sgsmLogicalOrigins[key] = logicalOriginSec
        }
        val ratio = stretchRatio(currentBpm, metaBpm)
        val started = nativeSgsmStart(
            name, metaSampleRate, ratio, gen, logicalOriginSec, durationSec
        )
        if (!started) synchronized(stateLock) {
            startedSgsm.remove(key)
            sgsmLogicalOrigins.remove(key)
        }
        return started
    }

    // ================= 解码会话 =================

    /**
     * 为 Signalsmith 或 SBSMS 建立可从任意原始时间点起播的解码流。
     *
     * 起点为 0 时直接顺序解码；否则先解码 [start, end)，再解码 [0, start)，
     * 让输出缓冲的第 0 帧正好对应 cut point，同时仍包含一整轮可循环音频。
     */
    private fun createStreamingDecoder(name: String, sourceStartSec: Double): PcmChunkSource? {
        val bytes = synchronized(opusData) { opusData[name] } ?: return null
        return try {
            val startUs = (sourceStartSec.coerceAtLeast(0.0) * 1_000_000.0).toLong()
            if (startUs <= 0L) OpusDecoder(bytes) else RotatedOpusDecoder(bytes, startUs)
        } catch (e: Exception) {
            Log.e(TAG, "create streaming decoder failed: $name start=$sourceStartSec", e)
            null
        }
    }

    /** 恢复已经停放的 SBSMS 输入端；native TempoStream 与 pendingBuf 始终保持原对象。 */
    private fun createHqResumeDecoder(name: String, session: HqSession): PcmChunkSource? {
        val bytes = synchronized(opusData) { opusData[name] } ?: return null
        return try {
            if (session.layout == HqLayout.HEAD) {
                OpusDecoder(bytes, session.resumeSourceUs, null)
            } else {
                RotatedOpusDecoder(
                    bytes,
                    session.rotationSplitUs,
                    initialCursorUs = session.resumeSourceUs,
                    initiallyWrapped = session.resumeWrapped
                )
            }
        } catch (e: Exception) {
            Log.e(TAG, "resume HQ decoder failed: $name", e)
            null
        }
    }

    /** 关闭昂贵的 MediaCodec，但保留可恢复游标以及 native SBSMS 流。 */
    private fun parkHqSession(name: String) {
        val session = synchronized(stateLock) { hqSessions[name] } ?: return
        val decoder = session.decoder ?: return
        session.resumeSourceUs = decoder.sourceCursorUs
        session.resumeWrapped = decoder.hasWrapped
        session.decoder = null
        decoder.release()
        Log.d(TAG, "park HQ $name layout=${session.layout} state=" +
            "${synchronized(stateLock) { states[name] ?: STATE_NONE }} " +
            "sourceUs=${session.resumeSourceUs} wrapped=${session.resumeWrapped}")
    }

    private fun removeHqSession(name: String) {
        val session = synchronized(stateLock) { hqSessions.remove(name) }
        session?.decoder?.release()
    }

    /** generation 切换可来自主线程；释放动作排到 HQ 队列，避免与正在 nextChunk 的任务并发。 */
    private fun releaseAllHqSessions() {
        val list = synchronized(stateLock) {
            val l = hqSessions.values.toList()
            hqSessions.clear()
            l
        }
        if (list.isNotEmpty()) {
            hqExec.execute {
                list.forEach { session -> runCatching { session.decoder?.release() } }
            }
        }
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

    private interface PcmChunkSource {
        val sampleRate: Int
        /** 下一次重新解码时应使用的原始音频时间戳。 */
        val sourceCursorUs: Long
        /** 旋转流是否已经从尾段绕回原始音频开头。 */
        val hasWrapped: Boolean
        fun nextChunk(maxFrames: Int): ShortArray?
        fun release()
    }

    /**
     * 可恢复的 Opus 解码器。startUs/endUs 使用原始音频时间轴；seek 到前一个同步点后，
     * 通过输出 PTS 丢弃边界外的 PCM，所以分段拼接不会把 seek 前的音频带入结果。
     */
    private class OpusDecoder(
        data: ByteArray,
        private val startUs: Long = 0L,
        private val endUs: Long? = null
    ) : PcmChunkSource {
        private val extractor = MediaExtractor()
        private val codec: MediaCodec
        private val info = MediaCodec.BufferInfo()
        private val chunkBytes = ByteArrayOutputStream()
        override val sampleRate: Int
        private var inputDone = false
        private var outputDone = false
        private var released = false
        @Volatile private var emittedEndUs = startUs
        override val sourceCursorUs: Long get() = emittedEndUs
        override val hasWrapped: Boolean get() = false

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
            if (startUs > 0L) {
                extractor.seekTo(startUs, MediaExtractor.SEEK_TO_PREVIOUS_SYNC)
            }
            codec = MediaCodec.createDecoderByType(format.getString(MediaFormat.KEY_MIME)!!)
            codec.configure(format, null, null, 0)
            codec.start()
        }

        @Synchronized
        override fun nextChunk(maxFrames: Int): ShortArray? {
            if (released) return null
            if (outputDone) return null
            chunkBytes.reset()
            val maxBytes = maxFrames * 2 * 2  // int16 交错立体声
            while (chunkBytes.size() < maxBytes && !outputDone) {
                if (!inputDone) {
                    val inIdx = codec.dequeueInputBuffer(5000)
                    if (inIdx >= 0) {
                        val buf = codec.getInputBuffer(inIdx)!!
                        val sampleTime = extractor.sampleTime
                        if (sampleTime < 0L || (endUs != null && sampleTime >= endUs)) {
                            codec.queueInputBuffer(inIdx, 0, 0, 0, MediaCodec.BUFFER_FLAG_END_OF_STREAM)
                            inputDone = true
                        } else {
                            val n = extractor.readSampleData(buf, 0)
                            if (n < 0) {
                                codec.queueInputBuffer(
                                    inIdx, 0, 0, 0, MediaCodec.BUFFER_FLAG_END_OF_STREAM
                                )
                                inputDone = true
                            } else {
                                codec.queueInputBuffer(inIdx, 0, n, sampleTime, 0)
                                extractor.advance()
                            }
                        }
                    }
                }
                when (val outIdx = codec.dequeueOutputBuffer(info, 5000)) {
                    MediaCodec.INFO_OUTPUT_FORMAT_CHANGED, MediaCodec.INFO_TRY_AGAIN_LATER -> {}
                    else -> {
                        if (outIdx >= 0) {
                            val buf = codec.getOutputBuffer(outIdx)!!
                            if (info.size > 0) {
                                // 当前工程的 Opus 均为 int16 交错立体声，每帧 4 字节。
                                val frameBytes = 4
                                val frames = info.size / frameBytes
                                val ptsUs = info.presentationTimeUs
                                val firstFrame = if (ptsUs < startUs) {
                                    kotlin.math.ceil(
                                        (startUs - ptsUs).toDouble() * sampleRate / 1_000_000.0
                                    ).toInt().coerceIn(0, frames)
                                } else 0
                                val lastFrame = endUs?.let { end ->
                                    kotlin.math.ceil(
                                        (end - ptsUs).toDouble() * sampleRate / 1_000_000.0
                                    ).toInt().coerceIn(0, frames)
                                } ?: frames
                                val keptFrames = (lastFrame - firstFrame).coerceAtLeast(0)
                                if (keptFrames > 0) {
                                    val byteOffset = info.offset + firstFrame * frameBytes
                                    val byteCount = keptFrames * frameBytes
                                    buf.position(byteOffset)
                                    buf.limit(byteOffset + byteCount)
                                    val bytes = ByteArray(byteCount)
                                    buf.get(bytes)
                                    chunkBytes.write(bytes)
                                    val decodedEndUs = ptsUs +
                                        (firstFrame + keptFrames).toLong() * 1_000_000L /
                                        sampleRate.toLong()
                                    emittedEndUs = maxOf(
                                        emittedEndUs,
                                        endUs?.let { minOf(it, decodedEndUs) } ?: decodedEndUs
                                    )
                                }
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

        @Synchronized
        override fun release() {
            if (released) return
            released = true
            runCatching { codec.stop() }
            runCatching { codec.release() }
            runCatching { extractor.release() }
        }
    }

    /** 将同一 Opus 的尾段和头段表现为一条连续 PCM 流。 */
    private class RotatedOpusDecoder(
        private val data: ByteArray,
        private val splitUs: Long,
        initialCursorUs: Long = splitUs,
        initiallyWrapped: Boolean = false
    ) : PcmChunkSource {
        private var phase = if (initiallyWrapped) 1 else 0
        private var cursorUs = initialCursorUs
        private var decoder: OpusDecoder? = if (phase == 0) {
            OpusDecoder(data, initialCursorUs, null)
        } else {
            OpusDecoder(data, initialCursorUs, splitUs)
        }
        override val sampleRate: Int = decoder?.sampleRate ?: 48000
        override val sourceCursorUs: Long get() = decoder?.sourceCursorUs ?: cursorUs
        override val hasWrapped: Boolean get() = phase == 1

        @Synchronized
        override fun nextChunk(maxFrames: Int): ShortArray? {
            while (true) {
                val current = decoder ?: return null
                val chunk = current.nextChunk(maxFrames)
                cursorUs = current.sourceCursorUs
                if (chunk != null) return chunk
                current.release()
                if (phase == 0 && splitUs > 0L) {
                    phase = 1
                    cursorUs = 0L
                    decoder = OpusDecoder(data, 0L, splitUs)
                } else {
                    decoder = null
                    return null
                }
            }
        }

        @Synchronized
        override fun release() {
            decoder?.let {
                cursorUs = it.sourceCursorUs
                it.release()
            }
            decoder = null
        }
    }

    // ================= variation 键解析 =================

    /**
     * 只保留前端主表格、Fill、Ending 以及两个精确特殊段落。
     * INTRO_01/BRIDGE/PICKUPS 等既不加载，也不进入任何渲染或 RMS 排名。
     */
    private fun isSupportedVariationKey(key: String): Boolean {
        val normalized = key.uppercase()
        if (normalized == "INTRO" || normalized == "BREAK") return true
        return categoryOf(normalized) in MAIN_VARIATION_CATEGORIES ||
            categoryOf(normalized) == "fill" || categoryOf(normalized) == "ending"
    }

    private fun categoryOf(key: String): String {
        val parts = key.split("_")
        val head = parts.firstOrNull() ?: return "other"
        return when (head) {
            "VERSE" -> if (parts.size > 2 && parts[1].matches(Regex("\\d+"))) "verse2" else "verse"
            "PRE" -> "prechorus"
            "CHORUS" -> "chorus"
            "FILLS" -> "fill"
            "INTRO" -> "intro"
            "BREAK" -> "break"
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
        "break" -> 6
        "bridge" -> 7
        "ending" -> 8
        "pickup" -> 9
        else -> 10
    }

    private fun jsEscape(s: String): String = s.replace("\\", "\\\\").replace("'", "\\'")
}
