#include <jni.h>
#include <string>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <condition_variable>
#include <set>
#include <thread>
#include <vector>
#include <android/log.h>
#include "AudioEngine.h"
#include "MidiProcessor.h"
#include "StylePlayer.h"
#include "ChordDetector.h"
// #include "TempoTracker.h" — removed, rewriting tempo detection
#include "BeatTracker.h"
#include "RhythmAudioEngine.h"
#include "CajonAssistant.h"
#include "TempoDetector.h"
#include "BassAssist.h"
#include "DrumLoopEngine.h"

static AudioEngine audio;
static MidiProcessor midi;
static StylePlayer stylePlayer;
static ChordDetector chordDetector;
// static TempoTracker tempoTracker; — removed, rewriting tempo detection
static BeatTracker beatTracker;
extern RhythmAudioEngine* g_rhythmEngine;
static RhythmAudioEngine rhythmEngine;
CajonAssistant cajon;
static TempoDetector tempoDetector;
static BassAssist bassAssist;
extern DrumLoopEngine* g_drumLoopEngine;
static DrumLoopEngine drumLoopEngine;
std::atomic<float> g_bassVolume{0.8f};  // AudioEngine 混音用
std::atomic<bool> g_bassLoopMode{false};
static std::atomic<int> assistType{0};
float g_minCajonEnergy = 0.0f;
std::atomic<double> g_pendingBpmUpdate{-1.0};
std::atomic<double> g_pendingRhythmGainUpdate{-1.0};
std::atomic<double> g_pendingMinEnergyUpdate{-1.0};
static std::atomic<int> g_pendingTempoHighlight{0};
static std::atomic<int> g_pendingScatterUpdate{0};
static std::atomic<int> g_pendingTempoFlash{0};

// ===== 鼓循环: beat-0 切换请求 =====
static std::mutex g_drumSwitchMutex;
static std::string g_pendingDrumSwitch;           // 待切换 variation (下一小节第一拍触发)
static std::string g_drumSwitchEventName;         // 已触发的切换名 (Kotlin 轮询)
static std::atomic<int> g_drumSwitchEvent{0};

// 相位调试
static std::atomic<double> g_measureStartMs{0};
static bool g_syncJustTriggered = false;
static std::atomic<int> g_pendingChordUpdate{0};
static std::atomic<float> g_pendingBassVolUpdate{-1.0f};
static std::atomic<int> g_bpmPrepPending{0};      // 1 = 有待生效目标；鼓循环走 cut point，其余路径走第一拍
static std::atomic<double> g_bpmPrepTarget{-1.0}; // 目标 BPM；-1 = 当前没有待处理目标
static std::atomic<int> g_bpmPrepEvent{0};        // native 来源待 Kotlin 轮询的预备请求
static std::mutex g_bpmPrepMutex;                 // 串行化连续 +/- 与拍头应用
static uint64_t g_measureSerial = 0;              // 已开始的小节序号 (由 step 0 递增)
static uint64_t g_bpmPrepEarliestMeasure = 0;     // 最早允许正式变速的小节序号
static bool g_bpmAdjustHeld = false;              // +/- 指针按住期间禁止拍头正式变速
static bool g_bpmPrepDispatched = false;          // Kotlin 已为最新目标启动预备流程
static constexpr double kBpmPrepTailFraction = 0.125;
static std::string g_chordDisplayStr;
// 和弦显示继续反映即时识别结果；贝斯和弦额外用低音区集合过滤松键过程中
// 出现的 C -> Em -> G 一类短暂子集。这里必须和 ChordDetector 一起串行化，
// 因为 NoteOn/NoteOff、CC64 与界面设置可能来自不同线程。
static std::mutex g_bassChordPushMutex;
static std::set<int> g_previousLowNotes;
static bool g_previousLowNotesValid = false;
static bool g_bassChordPushDeferred = false;
static bool g_syncResetting = false;
static std::vector<double> g_recentSyncErrs;

// BassAssist debug
static int g_bassLowPitch = -1;
static int g_bassLowVel = 0;
static double g_bassLowTime = 0;

// ===== Cajon 能量: 根据近 2 秒 MIDI 音符密度自动计算 =====
static std::vector<double> recentNoteTimestamps;
static std::mutex energyMutex;
// ===== C++ → Kotlin push 通道 =====
static JavaVM* g_jvm = nullptr;
static jobject g_activityObj = nullptr;
static std::mutex g_tempoCutDispatchWaitMutex;
static std::condition_variable g_tempoCutDispatchCv;
static std::atomic<uint64_t> g_tempoCutDispatchSerial{0};
static std::atomic<double> g_tempoCutDispatchTarget{-1.0};
static std::once_flag g_tempoCutDispatchOnce;

// 专用非实时线程：音频回调只做原子写入 + notify；JNI/StylePlayer 更新在这里执行，
// 使 Kotlin 能立即解锁串行变速，同时不把 JVM 调用放进 Oboe 回调。
static void drumTempoCutDispatchLoop() {
    uint64_t seen = 0;
    for (;;) {
        std::unique_lock<std::mutex> lk(g_tempoCutDispatchWaitMutex);
        g_tempoCutDispatchCv.wait(lk, [&] {
            return g_tempoCutDispatchSerial.load(std::memory_order_acquire) != seen;
        });
        seen = g_tempoCutDispatchSerial.load(std::memory_order_acquire);
        const double target = g_tempoCutDispatchTarget.load(std::memory_order_acquire);
        lk.unlock();

        // 清掉 150 ms 轮询兜底事件；若轮询抢先领取，Kotlin 的 active/target 校验会忽略重复通知。
        drumLoopEngine.getAndClearTempoCutEvent();
        stylePlayer.setTempoBPM(target);
        if (!g_jvm || !g_activityObj || target <= 0.0) continue;

        JNIEnv* env = nullptr;
        bool attached = false;
        if (g_jvm->GetEnv((void**)&env, JNI_VERSION_1_6) == JNI_EDETACHED) {
            if (g_jvm->AttachCurrentThread(&env, nullptr) != JNI_OK) continue;
            attached = true;
        }
        jclass cls = env->GetObjectClass(g_activityObj);
        jmethodID mid = cls
                ? env->GetMethodID(cls, "onNativeDrumTempoCutApplied", "(D)V")
                : nullptr;
        if (mid) env->CallVoidMethod(g_activityObj, mid, (jdouble)target);
        if (env->ExceptionCheck()) {
            env->ExceptionDescribe();
            env->ExceptionClear();
        }
        if (cls) env->DeleteLocalRef(cls);
        if (attached) g_jvm->DetachCurrentThread();
    }
}

static void startDrumTempoCutDispatchThread() {
    std::call_once(g_tempoCutDispatchOnce, [] {
        std::thread(drumTempoCutDispatchLoop).detach();
    });
}

static double clampBpm(double bpm) {
    if (bpm < 30.0) return 30.0;
    if (bpm > 300.0) return 300.0;
    return bpm;
}

static double steadyNowMs() {
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration<double, std::milli>(now).count();
}

// 必须在 g_bpmPrepMutex 内调用。预备发生在小节最后 1/8 时，跳过紧邻的拍头。
static uint64_t calculateBpmPrepEarliestMeasureLocked() {
    const double bpm = beatTracker.getCurrentBpm();
    const double measureMs = bpm > 0.0 ? 240000.0 / bpm : 0.0; // 当前调度固定 4/4
    const double startMs = g_measureStartMs.load();
    const double elapsedMs = steadyNowMs() - startMs;
    const bool inTailWindow = startMs <= 0.0 || measureMs <= 0.0 || elapsedMs < 0.0 ||
        elapsedMs >= measureMs * (1.0 - kBpmPrepTailFraction);
    return g_measureSerial + (inTailWindow ? 2u : 1u);
}

// g_bpmPrepMutex 必须由调用者持有。notifyKotlin=false 用于 WebView +/- 的同步桥接路径：
// MainActivity 会在同一次调用链中立即派发预备流程，无需等待 150 ms 状态轮询。
static void setBpmPrepTargetLocked(double target, bool notifyKotlin = true) {
    g_bpmPrepTarget.store(clampBpm(target));
    g_bpmPrepPending.store(1);
    g_bpmPrepEvent.store(notifyKotlin ? 1 : 0);
    g_bpmPrepDispatched = false;
    g_bpmPrepEarliestMeasure = calculateBpmPrepEarliestMeasureLocked();
}

static void requestBpmPrep(double target) {
    std::lock_guard<std::mutex> lk(g_bpmPrepMutex);
    setBpmPrepTargetLocked(target);
}

// Oboe 音频回调在 cut point 精确切换缓冲后调用。这里只更新节拍器/原子状态并唤醒
// 专用分发线程；JNI 和 StylePlayer 更新不会在实时回调内执行。
static void onDrumTempoCutApplied(double target) {
    beatTracker.setTempo(target);
    g_pendingBpmUpdate.store(target);
    double expected = target;
    if (g_bpmPrepTarget.compare_exchange_strong(expected, -1.0)) {
        g_bpmPrepPending.store(0);
    }
    g_tempoCutDispatchTarget.store(target, std::memory_order_release);
    g_tempoCutDispatchSerial.fetch_add(1, std::memory_order_acq_rel);
    g_tempoCutDispatchCv.notify_one();
    __android_log_print(ANDROID_LOG_INFO, "BeatTracker",
                        "BPM applied at audio cut point: %.1f", target);
}

// 一次性 INTRO/FILL/BREAK 播完切到目标，或延迟 ENDING 在小节边界启动时，
// 把节拍器同步到新段落第一拍。这里只更新 BeatTracker 的无锁时间基准。
static void onDrumOneShotSwitched() {
    beatTracker.sync();
}

// DrumLoopEngine 只负责 MIDI transport；所有声音仍由与箱鼓贝斯助手相同的
// AudioEngine bass synth、SoundFont、增益和混响链生成。
static void onDrumBassMidiEvent(int action, int pitch, int velocity) {
    if (action == DrumLoopEngine::BassNoteOn && pitch >= 0 && velocity > 0) {
        audio.enqueueNoteOn(2, 0, pitch, velocity);
    } else if (action == DrumLoopEngine::BassNoteOff && pitch >= 0) {
        audio.enqueueNoteOff(2, 0, pitch);
    } else if (action == DrumLoopEngine::BassAllSoundsOff) {
        audio.enqueueAllSoundsOff(2);
    }
}

// MIDI 输入线程在音符/踏板状态改变后直接调用：ChordDetector -> 贝斯循环，
// 全程停留在原生 C++ 调用栈中，不等待 WebView 状态轮询。
static bool detectAndPushChordToBassLoop() {
    std::lock_guard<std::mutex> stateLock(g_bassChordPushMutex);
    const MidiNoteSetSnapshot snapshot = midi.getNoteSetSnapshot();
    // std::includes 也把“完全相等”算作子集：只有出现了至少一个新的低音区
    // 音符才算真实切换；单纯减少（或低音区未变）不会推动贝斯和弦。
    const bool lowNotesAreSubset = g_previousLowNotesValid &&
        std::includes(g_previousLowNotes.begin(), g_previousLowNotes.end(),
                      snapshot.lowNotes.begin(), snapshot.lowNotes.end());
    const size_t previousLowCount = g_previousLowNotes.size();
    g_previousLowNotes = snapshot.lowNotes;
    g_previousLowNotesValid = true;

    chordDetector.detect(snapshot.allNotes);
    const bool chordChanged = chordDetector.changed();
    if (chordChanged) {
        g_chordDisplayStr = chordDetector.getChord();
        g_pendingChordUpdate.store(1);
    }

    const int bassPitch = BassAssist::getBassPitchFromChord(chordDetector.getChord());
    const bool validBassChord = bassPitch >= 0;
    bool bassContextPushed = false;
    if (chordChanged && lowNotesAreSubset && validBassChord) {
        // 没有加入新低音区音符时，更新全局显示但保留贝斯和弦。后续一旦加入
        // 新音，即使识别出的和弦名没有再次变化，也会补推这次真实切换。
        g_bassChordPushDeferred = true;
        __android_log_print(ANDROID_LOG_DEBUG, "DrumLoop",
                            "defer subset bass chord %s low=%zu prev=%zu",
                            chordDetector.getChord().c_str(), snapshot.lowNotes.size(),
                            previousLowCount);
    } else if ((chordChanged || g_bassChordPushDeferred) &&
               (!lowNotesAreSubset || !validBassChord)) {
        drumLoopEngine.setBassChordContext(
            bassPitch, chordDetector.getRootMidi(), chordDetector.getChordNotes(),
            beatTracker.getCurrentBpm());
        g_bassChordPushDeferred = false;
        bassContextPushed = true;
    }
    return chordChanged || bassContextPushed;
}

static void pushSustainToKotlin(int cc) {
    if (!g_jvm || !g_activityObj || cc < 0) return;
    JNIEnv* env;
    bool attached = false;
    if (g_jvm->GetEnv((void**)&env, JNI_VERSION_1_6) == JNI_EDETACHED) {
        g_jvm->AttachCurrentThread(&env, nullptr);
        attached = true;
    }
    jclass cls = env->GetObjectClass(g_activityObj);
    jmethodID mid = env->GetMethodID(cls, "onNativeSustainCC", "(I)V");
    if (mid) env->CallVoidMethod(g_activityObj, mid, cc);
    if (attached) g_jvm->DetachCurrentThread();
}

static void pushBeatDotUpdate() {
    if (!g_jvm || !g_activityObj) return;
    JNIEnv* env;
    bool attached = false;
    if (g_jvm->GetEnv((void**)&env, JNI_VERSION_1_6) == JNI_EDETACHED) {
        g_jvm->AttachCurrentThread(&env, nullptr);
        attached = true;
    }
    jclass cls = env->GetObjectClass(g_activityObj);
    jmethodID mid = env->GetMethodID(cls, "onNativeBeatUpdate", "()V");
    if (mid) env->CallVoidMethod(g_activityObj, mid);
    if (attached) g_jvm->DetachCurrentThread();
}

static void pushEnergyDisplayUpdate() {
    if (!g_jvm || !g_activityObj) return;
    JNIEnv* env;
    bool attached = false;
    if (g_jvm->GetEnv((void**)&env, JNI_VERSION_1_6) == JNI_EDETACHED) {
        g_jvm->AttachCurrentThread(&env, nullptr);
        attached = true;
    }
    jclass cls = env->GetObjectClass(g_activityObj);
    jmethodID mid = env->GetMethodID(cls, "onNativeEnergyUpdate", "()V");
    if (mid) env->CallVoidMethod(g_activityObj, mid);
    if (attached) g_jvm->DetachCurrentThread();
}

static void onBeatStep(int step, double bpm, void*) {
    const double ms = steadyNowMs();
    // 预备变速只能在满足锁和安全窗口条件的第一拍正式执行。
    if (step == 0) {
        double target = -1.0;
        bool deferBpmSwitch = false;
        bool held = false;
        bool dispatched = false;
        uint64_t measureSerial = 0;
        uint64_t earliestMeasure = 0;
        const bool drumLoopPlaying = drumLoopEngine.isPlaying();
        {
            std::lock_guard<std::mutex> lk(g_bpmPrepMutex);
            measureSerial = ++g_measureSerial;
            g_measureStartMs.store(ms);
            if (g_bpmPrepPending.load()) {
                if (drumLoopPlaying) {
                    // 鼓循环播放中的 BPM 由 DrumLoopEngine 在任意 cut point 应用；
                    // 第一拍只继续负责普通 variation 切换，不能抢先改变速度。
                    deferBpmSwitch = false;
                } else {
                    held = g_bpmAdjustHeld;
                    dispatched = g_bpmPrepDispatched;
                    earliestMeasure = g_bpmPrepEarliestMeasure;
                    deferBpmSwitch = held || !dispatched || measureSerial < earliestMeasure;
                    if (!deferBpmSwitch) {
                        g_bpmPrepPending.store(0);
                        g_bpmPrepDispatched = false;
                        target = g_bpmPrepTarget.exchange(-1.0);
                    }
                }
            }
        }

        // BPM 仍被长按锁/末尾安全窗推迟时，同一次预备产生的 variation 切换也必须保留。
        // 否则会把尚未完成的目标速度缓冲切到播放槽，随后又被下一次长按请求取消。
        if (!deferBpmSwitch) {
            std::lock_guard<std::mutex> lk(g_drumSwitchMutex);
            if (!g_pendingDrumSwitch.empty()) {
                std::string sw = g_pendingDrumSwitch;
                g_pendingDrumSwitch.clear();
                if (g_drumLoopEngine) {
                    g_drumLoopEngine->play(sw);
                    g_drumSwitchEventName = sw;
                    g_drumSwitchEvent.store(1);
                    __android_log_print(ANDROID_LOG_INFO, "DrumLoop",
                                        "beat-0 switch to %s", sw.c_str());
                }
            }
        } else {
            __android_log_print(ANDROID_LOG_INFO, "BeatTracker",
                                "BPM deferred at beat-0: held=%d dispatched=%d measure=%llu earliest=%llu",
                                held ? 1 : 0, dispatched ? 1 : 0,
                                (unsigned long long)measureSerial,
                                (unsigned long long)earliestMeasure);
        }
        if (target >= 0.0) {
            beatTracker.setTempo(target);
            stylePlayer.setTempoBPM(target);
            g_pendingBpmUpdate.store(target);
            __android_log_print(ANDROID_LOG_INFO, "BeatTracker",
                                "BPM applied at beat-0: %.1f", target);
        }
    }
    int bassVel = cajon.processStep(step, bpm);

    float autoEnergy = cajon.updateEnergy(ms, beatTracker.getCurrentBpm());
    float energy = std::max(autoEnergy, g_minCajonEnergy);
    cajon.setEnergy(energy);
    drumLoopEngine.setBassSelectionEnergy(energy);

    // 箱鼓模式使用动态触发的 BassAssist；鼓循环模式改由同名 MIDI 乐句驱动，
    // 两者最终都进入 AudioEngine 的 bass synth，不能同时触发。
    if (assistType.load(std::memory_order_relaxed) == 0) {
        bassAssist.processStep(step, bassVel, cajon.getLastToneVel(), ms, bpm,
                               g_chordDisplayStr, chordDetector.getChordNotes(),
                               energy, audio, g_rhythmEngine);
    }
    pushEnergyDisplayUpdate();
    // 每拍首子步推送白点更新
    if (step % 8 == 0) pushBeatDotUpdate();
}

// REMOVED: updateCajonEnergy moved to CajonAssistant
static void __unused_old_updateCajonEnergy(double nowMs) {
    std::lock_guard<std::mutex> lock(energyMutex);
    // 清理超过 2 秒的旧时间戳
    double cutoff = nowMs - 2000.0;
    recentNoteTimestamps.erase(
        std::remove_if(recentNoteTimestamps.begin(), recentNoteTimestamps.end(),
            [cutoff](double t) { return t < cutoff; }),
        recentNoteTimestamps.end());
    // 线性映射: 0 个 → 0.0, 20 个 → 1.0
    // BPM 归一化: 基准 70 BPM, 除数 = 24 * (bpm/70)
    double bpmFactor = beatTracker.getCurrentBpm() / 70.0;
    if (bpmFactor < 0.5) bpmFactor = 0.5;
    float autoEnergy = std::min(1.0f, (float)(recentNoteTimestamps.size() / (24.0 * bpmFactor)));
    float energy = std::max(autoEnergy, g_minCajonEnergy);
    cajon.setEnergy(energy);
    pushEnergyDisplayUpdate();
}

// ===== Audio Engine (FluidLite) =====
extern "C" JNIEXPORT void JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeInit(
        JNIEnv* env, jobject thiz, jstring sf2Path) {
    env->GetJavaVM(&g_jvm);
    g_activityObj = env->NewGlobalRef(thiz);
    const char *path = env->GetStringUTFChars(sf2Path, nullptr);
    srand((unsigned int)std::chrono::steady_clock::now().time_since_epoch().count());
    audio.init(path);
    // Init rhythm engine BEFORE audio starts (callback needs g_rhythmEngine set)
    g_rhythmEngine = &rhythmEngine;
    cajon.init(&rhythmEngine);
    g_drumLoopEngine = &drumLoopEngine;
    drumLoopEngine.setTempoCutCallback(onDrumTempoCutApplied);
    drumLoopEngine.setOneShotSwitchCallback(onDrumOneShotSwitched);
    drumLoopEngine.setBassMidiCallback(onDrumBassMidiEvent);
    drumLoopEngine.setBassChordContext(-1, -1, {}, beatTracker.getCurrentBpm());
    startDrumTempoCutDispatchThread();
    audio.start();
    // Beat tracker
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    double ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    // tempoTracker.init(4, 4, 75.0, ms); — removed
    beatTracker.setBeatZeroSignal(&stylePlayer.beatZeroSignal);
    beatTracker.setStepCallback(onBeatStep, nullptr);
    beatTracker.start(4, 75.0);
    env->ReleaseStringUTFChars(sf2Path, path);
}

extern "C" JNIEXPORT void JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeNoteOn(
        JNIEnv*, jobject, jint note, jint velocity) {
    audio.enqueueNoteOn(0, 0, note, velocity);
}

extern "C" JNIEXPORT void JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeNoteOff(
        JNIEnv*, jobject, jint note) {
    audio.enqueueNoteOff(0, 0, note);
}

extern "C" JNIEXPORT void JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeNoteOnBass(
        JNIEnv*, jobject, jint note, jint velocity) {
    audio.enqueueNoteOn(2, 0, note, velocity);
}

extern "C" JNIEXPORT void JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeNoteOffBass(
        JNIEnv*, jobject, jint note) {
    audio.enqueueNoteOff(2, 0, note);
}

extern "C" JNIEXPORT void JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeLoadBassSoundFont(
        JNIEnv* env, jobject, jstring sf2Path) {
    const char* path = env->GetStringUTFChars(sf2Path, nullptr);
    audio.loadSoundFont(2, path);
    env->ReleaseStringUTFChars(sf2Path, path);
    // 确保 Bass 音色: Bank 0, Program 33
    audio.enqueueProgramChange(2, 0, 0, 33);
}

extern "C" JNIEXPORT void JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeSendCC(
        JNIEnv*, jobject, jint controller, jint value) {
    // CC86 → lead gain (0-6.0)
    if (controller == 86) { audio.enqueueSetGain(0, value / 127.0 * 5.0); return; }
    // CC87 → 根据模式路由: type=0 控制 Cajon 音量, type=1 控制鼓循环/伴奏音量
    if (controller == 87) {
        if (assistType.load(std::memory_order_relaxed) == 0 && g_rhythmEngine) {
            float gain = value / 127.0f * 4.0f;
            g_rhythmEngine->setMasterGain(gain);
            g_pendingRhythmGainUpdate.store(gain);
        } else {
            audio.enqueueSetGain(1, value / 127.0 * 6.0);
        }
        return;
    }
    // CC89 → Cajon 能量 (0-127 → 0.0-1.0)
    if (controller == 89) {
        g_minCajonEnergy = value / 127.0f;
        g_pendingMinEnergyUpdate.store(g_minCajonEnergy);
        return;
    }
    // CC90 → BPM 调整 (锚点比例式, 3s 超时, 倍率固定 0.5x–2.0x)
    if (controller == 90) {
        static int anchor = -1;
        static double baseBpm = 75.0;
        static double lastTouch = 0;
        auto now = std::chrono::steady_clock::now().time_since_epoch();
        double nowSec = std::chrono::duration<double>(now).count();
        __android_log_print(ANDROID_LOG_INFO, "CC90", "value=%d anchor=%d baseBpm=%.0f", value, anchor, baseBpm);

        // 超过 3 秒未收到 CC90 → 复位锚点, 恢复 UI
        if (anchor >= 0 && (nowSec - lastTouch) > 3.0) {
            anchor = -1;
            // UI restore handled by Kotlin postDelayed
        }

        if (anchor < 0) {
            anchor = value;
            baseBpm = beatTracker.getCurrentBpm();
        } else if (value != anchor) {
            // 平方曲线: x ∈ [-1, 1], ratio = 1.5^(x*|x|)
            double x;
            if (value >= anchor) {
                x = (127 - anchor > 0) ? (double)(value - anchor) / (127 - anchor) : 0.0;
            } else {
                x = (anchor - 1 > 0) ? -(double)(anchor - value) / (anchor - 1) : 0.0;
            }
            double ratio = std::pow(1.5, std::copysign(std::pow(std::abs(x), 1.4), x));
            double newBpm = std::round(baseBpm * ratio);
            // 预备变速: 每次旋转都更新目标并触发预备 (不在旋转时立即应用)
            requestBpmPrep(newBpm);
        }
        lastTouch = nowSec;
        g_pendingTempoHighlight.store(1); // 每次 CC90 都重置 Kotlin 计时器
        return;
    }
    audio.enqueueCC(0, 0, controller, value);
}

extern "C" JNIEXPORT void JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeRestartAudio(
        JNIEnv*, jobject) {
    audio.restartStream();
}

extern "C" JNIEXPORT void JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeLoadSoundFont(
        JNIEnv* env, jobject, jstring sf2Path) {
    const char *path = env->GetStringUTFChars(sf2Path, nullptr);
    audio.loadSoundFont(0, path);
    env->ReleaseStringUTFChars(sf2Path, path);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeGetInstrumentCount(
        JNIEnv*, jobject) { return audio.getInstrumentCount(0); }

extern "C" JNIEXPORT jstring JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeGetInstrumentName(
        JNIEnv* env, jobject, jint index) {
    return env->NewStringUTF(audio.getInstrumentName(0, index));
}

extern "C" JNIEXPORT void JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeSetInstrument(
        JNIEnv*, jobject, jint index) { audio.setInstrument(0, index); }

extern "C" JNIEXPORT jint JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeGetInstrumentBank(
        JNIEnv*, jobject, jint index) { return audio.getInstrumentBank(0, index); }

extern "C" JNIEXPORT void JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeSetMasterVolume(
        JNIEnv*, jobject, jfloat gain) { audio.setMasterVolume(0, gain); }

// ===== MIDI Processor (note state, auto-sustain, chord analysis) =====
// Encode sustainCC + bassNote + bassVelocity into one int for Kotlin
static jint encodeResult(int sustainCC, int bassNote, int bassVel) {
    return (sustainCC & 0xFF) | ((bassNote + 1) << 8) | ((bassVel & 0xFF) << 16);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeProcessNoteOn(
        JNIEnv*, jobject, jint note, jint velocity) {
    // 统计近 2 秒音符密度 → Cajon 能量
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    double ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    cajon.feedNoteOn(ms);

    tempoDetector.setSplitPoint(midi.getSplitNote());
    bool measureChange = tempoDetector.feedNoteOn(note, velocity, ms);
    g_pendingScatterUpdate.store(1);

    // 相位微调 + 调试输出
    // 相位微调
    double beatMs = 240000.0 / beatTracker.getCurrentBpm();
    double phaseOff = tempoDetector.getPhaseOffset();
    // offset: 从上次 beat 0 到当前音符的时间
    double startMs = g_measureStartMs.load();
    double syncOffset = (startMs > 0) ? (ms - startMs) : 0;
    tempoDetector.setSyncOffset(syncOffset);
    double chartMs = 240000.0 / tempoDetector.getBestBPM();
    double phaseMs = fmod(ms - phaseOff, chartMs);
    if (phaseMs < 0) phaseMs += chartMs;
    double syncErr = phaseMs - syncOffset;
    tempoDetector.setSyncError(syncErr);
    // 散点与节拍器偏差 → 连续 3 次稳定 → 对齐
    double measureMs = 240000.0 / beatTracker.getCurrentBpm();
    if (!g_syncResetting) {
        double normErr = fmod(syncErr, measureMs);
        if (normErr < 0) normErr += measureMs;
        g_recentSyncErrs.push_back(normErr);
        __android_log_print(ANDROID_LOG_INFO, "TempoDetector", "push err=%.0f size=%zu", normErr, g_recentSyncErrs.size());
        if (g_recentSyncErrs.size() > 3) g_recentSyncErrs.erase(g_recentSyncErrs.begin());
        double mean = 0;
        if (g_recentSyncErrs.size() == 3) {
            mean = (g_recentSyncErrs[0] + g_recentSyncErrs[1] + g_recentSyncErrs[2]) / 3.0;
            bool stable = true;
            for (double v : g_recentSyncErrs) {
                if (std::abs(v - mean) > 150.0) { stable = false; break; }
            }
            if (stable) {
                float rg = g_rhythmEngine ? g_rhythmEngine->getMasterGain() : 0.0f;
                if (rg <= 0.0f) {
                    g_syncResetting = true;
                    g_syncJustTriggered = true;
                    double delay = measureMs - syncOffset - mean;
                    while (delay < 0) delay += measureMs;
                    while (delay >= measureMs) delay -= measureMs;
                    int delayMs = (int)delay;
                    __android_log_print(ANDROID_LOG_INFO, "TempoDetector", "sync: mean=%.0f sweep=%.0f delay=%dms", mean, syncOffset, delayMs);
                    std::thread([delayMs]() {
                        std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
                        float gNow = g_rhythmEngine ? g_rhythmEngine->getMasterGain() : 0.0f;
                        if (gNow <= 0.0f) beatTracker.sync();
                        g_syncResetting = false;
                    }).detach();
                }
                g_recentSyncErrs.clear();
            }
        }
        tempoDetector.setSyncMean(mean);
    }

    // nudge 暂时禁用 — getPhaseMs() 与真实拍位不对齐, 待重写
    if (measureChange) {
        // 仅在 Cajon 音量为 0 时更新全局 Tempo
        float rhythmGain = g_rhythmEngine ? g_rhythmEngine->getMasterGain() : 0.0f;
        if (rhythmGain <= 0.0f) {
            double newBpm = tempoDetector.getBestBPM();
            int newRounded = (int)std::round(newBpm);
            int curRounded = (int)std::round(beatTracker.getCurrentBpm());
            if (newRounded != curRounded) {
                beatTracker.setTempo(newBpm);
                stylePlayer.setTempoBPM(newBpm);
                g_pendingBpmUpdate.store(newBpm);
                g_pendingTempoFlash.store(1);
                beatTracker.sync(); // 立即从第一拍开始
                g_syncResetting = false; // 取消 sleepFor 对齐任务
                tempoDetector.setSyncError(0.0);
            }
        }
    }

    auto r = midi.processNoteOn(note, velocity, ms);
    // 新和弦识别: 用 ActiveNotes ∪ PedalHeldNotes，并原生推送给贝斯循环。
    if (detectAndPushChordToBassLoop()) {
        std::string chordName = chordDetector.getChord();
        if (chordName != "-" && chordName != "???") {
            int root = chordDetector.getRootMidi();
            stylePlayer.setChordRoot(root, chordName);
        }
    }
    return encodeResult(r.sustainCCToSend, r.bassNote, r.bassVelocity);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeProcessNoteOff(
        JNIEnv*, jobject, jint note) {
    auto r = midi.processNoteOff(note);
    detectAndPushChordToBassLoop();
    return encodeResult(r.sustainCCToSend, r.bassNote, 0);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeProcessCC(
        JNIEnv*, jobject, jint controller, jint value) {
    auto r = midi.processCC(controller, value);
    // CC64 会改变 PedalHeldNotes；踏板松开导致的和弦变化也必须立即原生推送。
    if (controller == 64) detectAndPushChordToBassLoop();
    float bv = midi.getAndClearBassVolumeFromCC();
    if (bv >= 0.0f) {
        g_bassVolume.store(bv, std::memory_order_relaxed);
        bassAssist.setVolume(bv);
        g_pendingBassVolUpdate.store(bv);
    }
    return r.sustainCCToSend;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeSetAutoSustain(
        JNIEnv*, jobject, jboolean enabled) {
    int cc = midi.setAutoSustainEnabled(enabled);
    detectAndPushChordToBassLoop();
    pushSustainToKotlin(cc);
    return cc;
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeGetNoteState(
        JNIEnv* env, jobject) {
    return env->NewStringUTF(midi.getNoteStateJson().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeGetRomanFromChord(
        JNIEnv* env, jobject, jstring chordName) {
    const char* cname = env->GetStringUTFChars(chordName, nullptr);
    int rootPc = chordDetector.getRootMidi();
    if (rootPc >= 0) rootPc %= 12;
    std::string result = midi.getRomanFromChord(std::string(cname), rootPc);
    env->ReleaseStringUTFChars(chordName, cname);
    return env->NewStringUTF(result.c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeGetChordInfo(
        JNIEnv* env, jobject) {
    return env->NewStringUTF(midi.getChordInfo().c_str());
}

extern "C" JNIEXPORT void JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeSetBassEnhance(
        JNIEnv*, jobject, jboolean enabled) {
    midi.setBassEnhanceEnabled(enabled);
    if (enabled) bassAssist.setEnabled(false); // 互斥
}

extern "C" JNIEXPORT jint JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeChangeTranspose(
        JNIEnv*, jobject, jint delta) { return midi.changeTranspose(delta); }

extern "C" JNIEXPORT jint JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeChangeSplitPoint(
        JNIEnv*, jobject, jint delta) {
    const int splitPoint = midi.changeSplitPoint(delta);
    // 改分割点本身不应被下一次 MIDI 事件误判为“松键子集”。
    std::lock_guard<std::mutex> stateLock(g_bassChordPushMutex);
    g_previousLowNotes = midi.getNoteSetSnapshot().lowNotes;
    g_previousLowNotesValid = true;
    g_bassChordPushDeferred = false;
    return splitPoint;
}

// ===== Style / Rhythm Playback =====
extern "C" JNIEXPORT jstring JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeLoadStyle(
        JNIEnv* env, jobject, jstring styPath) {
    const char* path = env->GetStringUTFChars(styPath, nullptr);
    bool ok = stylePlayer.loadStyle(path);
    env->ReleaseStringUTFChars(styPath, path);
    if (!ok)
        return env->NewStringUTF("{\"error\":\"Failed to load style\"}");
    beatTracker.setBeatZeroSignal(&stylePlayer.beatZeroSignal);
    beatTracker.start(stylePlayer.getTimeSigNum(), stylePlayer.getTempoBPM());
    return env->NewStringUTF(stylePlayer.getScenesJson().c_str());
}

extern "C" JNIEXPORT void JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeStyleSelectScene(
        JNIEnv*, jobject, jint sceneIndex) {
    stylePlayer.selectScene(sceneIndex);
}

extern "C" JNIEXPORT void JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeStyleStart(
        JNIEnv*, jobject) {
    beatTracker.sync(); // 对齐到第 1 拍
    // tempoEvents.clear(); — removed
    stylePlayer.start(&audio, 1, 0); // 从头播放
}

extern "C" JNIEXPORT void JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeStopStyle(
        JNIEnv*, jobject) {
    stylePlayer.stop();
    // 立即杀死所有 voice, 确保旧的 midi 信号不残留
    audio.enqueueAllSoundsOff(1);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeIsStylePlaying(
        JNIEnv*, jobject) {
    return stylePlayer.isPlaying() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeGetCurrentScene(
        JNIEnv*, jobject) {
    return stylePlayer.getCurrentScene();
}

extern "C" JNIEXPORT jint JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeGetPendingScene(
        JNIEnv*, jobject) {
    return stylePlayer.getPendingScene();
}

extern "C" JNIEXPORT jdouble JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeGetStyleTempo(
        JNIEnv*, jobject) {
    // TempoTracker removed — rewriting tempo detection
    return stylePlayer.getTempoBPM();
}

extern "C" JNIEXPORT jdouble JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeGetCurrentBpm(
        JNIEnv*, jobject) {
    return beatTracker.getCurrentBpm();
}

extern "C" JNIEXPORT jdouble JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeGetAndClearPendingBpm(
        JNIEnv*, jobject) {
    return g_pendingBpmUpdate.exchange(-1.0);
}

extern "C" JNIEXPORT jdouble JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeGetAndClearPendingRhythmGain(
        JNIEnv*, jobject) {
    return g_pendingRhythmGainUpdate.exchange(-1.0);
}

extern "C" JNIEXPORT jdouble JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeGetAndClearPendingMinEnergy(
        JNIEnv*, jobject) {
    return g_pendingMinEnergyUpdate.exchange(-1.0);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeGetAndClearPendingTempoHighlight(
        JNIEnv*, jobject) {
    return g_pendingTempoHighlight.exchange(0);
}

extern "C" JNIEXPORT void JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeSetBassAssist(
        JNIEnv*, jobject, jboolean enabled) {
    bassAssist.setEnabled(enabled);
    if (enabled) {
        midi.setBassEnhanceEnabled(false); // 互斥
        audio.enqueueProgramChange(0, 1, 0, 33); // lead synth ch1 = Fingered Bass
    }
}
extern "C" JNIEXPORT jboolean JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeIsBassAssistEnabled(
        JNIEnv*, jobject) { return bassAssist.isEnabled() ? JNI_TRUE : JNI_FALSE; }

extern "C" JNIEXPORT void JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeSetBassAssistVolume(
        JNIEnv*, jobject, jfloat volume) {
    bassAssist.setVolume(volume);
    g_bassVolume.store(volume, std::memory_order_relaxed);
}
extern "C" JNIEXPORT jfloat JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeGetBassAssistVolume(
        JNIEnv*, jobject) { return bassAssist.getVolume(); }

extern "C" JNIEXPORT jint JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeGetAndClearPendingScatter(
        JNIEnv*, jobject) {
    return g_pendingScatterUpdate.exchange(0);
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeGetAndClearPendingChord(
        JNIEnv* env, jobject) {
    if (g_pendingChordUpdate.exchange(0)) {
        return env->NewStringUTF(g_chordDisplayStr.c_str());
    }
    return env->NewStringUTF("");
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeGetChordNotes(
        JNIEnv* env, jobject) {
    return env->NewStringUTF(chordDetector.getChordNotesString().c_str());
}

extern "C" JNIEXPORT jint JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeGetAndClearPendingTempoFlash(
        JNIEnv*, jobject) {
    return g_pendingTempoFlash.exchange(0);
}

extern "C" JNIEXPORT jfloat JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeGetAndClearPendingBassVolume(
        JNIEnv*, jobject) {
    return g_pendingBassVolUpdate.exchange(-1.0f);
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeGetTempoDetectorData(
        JNIEnv* env, jobject) {
    tempoDetector.pruneNoteEvents(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    const auto& events = tempoDetector.getNoteEvents();
    const auto& bpmList = tempoDetector.getBpmList();
    std::string json = "{\"bpm\":" + std::to_string(tempoDetector.getBestBPM()) +
        ",\"offset\":" + std::to_string(tempoDetector.getPhaseOffset()) +
        ",\"split\":" + std::to_string(midi.getSplitNote()) +
        ",\"anomaly\":" + std::to_string(tempoDetector.getAnomalyCount()) +
        ",\"bpmList\":[";
    for (size_t i = 0; i < bpmList.size(); i++) {
        if (i > 0) json += ",";
        json += std::to_string(bpmList[i]);
    }
    json += "],\"syncOff\":" + std::to_string(tempoDetector.getSyncOffset()) + ",\"syncErr\":" + std::to_string(tempoDetector.getSyncError()) + ",\"syncMean\":" + std::to_string(tempoDetector.getSyncMean()) + ",\"synced\":" + std::to_string(g_syncJustTriggered ? 1 : 0) + ",\"err1\":" + std::to_string(g_recentSyncErrs.size()>=1?(int)g_recentSyncErrs[0]:0) + ",\"err2\":" + std::to_string(g_recentSyncErrs.size()>=2?(int)g_recentSyncErrs[1]:0) + ",\"err3\":" + std::to_string(g_recentSyncErrs.size()>=3?(int)g_recentSyncErrs[2]:0) + ",\"lowP\":" + std::to_string(g_bassLowPitch) + ",\"lowV\":" + std::to_string(g_bassLowVel) + ",\"lowT\":" + std::to_string((int64_t)g_bassLowTime) + ",\"events\":[";
    for (size_t i = 0; i < events.size(); i++) {
        if (i > 0) json += ",";
        json += "[" + std::to_string((int)events[i].pitch) + "," +
            std::to_string((int)events[i].velocity) + "," +
            std::to_string((int64_t)(events[i].timeMs)) + "]";
    }
    json += "]}";
    return env->NewStringUTF(json.c_str());
}

extern "C" JNIEXPORT jint JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeGetTimeSig(
        JNIEnv*, jobject) {
    return stylePlayer.getTimeSigNum();
}

extern "C" JNIEXPORT jint JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeGetCurrentBeat(
        JNIEnv*, jobject) {
    return beatTracker.getCurrentBeat();
}

extern "C" JNIEXPORT void JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeSyncBeat(
        JNIEnv*, jobject) {
    float rg = g_rhythmEngine ? g_rhythmEngine->getMasterGain() : 0.0f;
    if (rg > 0.0f) {
        beatTracker.sync();
        pushBeatDotUpdate();
    } else {
        beatTracker.tapTempo(true);
    }
}

extern "C" JNIEXPORT jdouble JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeDrumLoopTapTempo(
        JNIEnv*, jobject) {
    return beatTracker.tapTempo(false);
}

extern "C" JNIEXPORT jdouble JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeRequestBpmMult(
        JNIEnv*, jobject, jint mult) {
    // 同步返回目标，MainActivity 会在同一调用链中启动 cut-point 预备流程，
    // 不必等待 150 ms 状态轮询。
    std::lock_guard<std::mutex> lk(g_bpmPrepMutex);
    double cur = g_bpmPrepPending.load()
        ? g_bpmPrepTarget.load()
        : beatTracker.getCurrentBpm();
    double target;
    if (mult == 2) target = cur * 2.0;
    else if (mult == -2) target = std::ceil(cur / 2.0);
    else target = cur;
    target = clampBpm(target);
    if (std::abs(target - cur) < 0.001) return -1.0;
    setBpmPrepTargetLocked(target, false);
    __android_log_print(ANDROID_LOG_INFO, "BeatTracker",
                        "BPM prep: mult %d → target %.1f", mult, target);
    return target;
}

extern "C" JNIEXPORT jdouble JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeRequestBpmDelta(
        JNIEnv*, jobject, jint delta) {
    if (delta != -1 && delta != 1) return -1.0;

    double target;
    bool applyImmediately;
    {
        std::lock_guard<std::mutex> lk(g_bpmPrepMutex);
        // 连续点击/长按时从尚未生效的目标继续累加，而不是反复读取旧 BPM。
        double base = g_bpmPrepPending.load()
            ? g_bpmPrepTarget.load()
            : std::round(beatTracker.getCurrentBpm());
        target = clampBpm(base + (double)delta);
        if (std::abs(target - base) < 0.001) return -1.0; // 已到 30/300 边界

        applyImmediately = !drumLoopEngine.isPlaying();
        // +/- 由 MainActivity 在 adjustBpm() 返回后直接启动预备渲染；不要再写入轮询事件，
        // 否则同一目标会在最多 150 ms 后被重复处理。新目标也会覆盖尚未领取的旧事件。
        setBpmPrepTargetLocked(target, false);
        if (applyImmediately) {
            g_bpmPrepPending.store(0);
            g_bpmPrepEarliestMeasure = 0;
        }
    }

    if (applyImmediately) {
        beatTracker.setTempo(target);
        stylePlayer.setTempoBPM(target);
        g_pendingBpmUpdate.store(target);
        __android_log_print(ANDROID_LOG_INFO, "BeatTracker",
                            "BPM delta %+d applied immediately → %.1f", delta, target);
    } else {
        __android_log_print(ANDROID_LOG_INFO, "BeatTracker",
                            "BPM delta %+d prepared → %.1f (apply at cut point)", delta, target);
    }
    return target;
}

extern "C" JNIEXPORT void JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeSetBpmHold(
        JNIEnv*, jobject, jboolean held) {
    std::lock_guard<std::mutex> lk(g_bpmPrepMutex);
    g_bpmAdjustHeld = held == JNI_TRUE;
    __android_log_print(ANDROID_LOG_INFO, "BeatTracker",
                        "BPM hold lock: %s", g_bpmAdjustHeld ? "on" : "off");
}

extern "C" JNIEXPORT jint JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeGetBeatIndex(
        JNIEnv*, jobject) {
    // TempoTracker removed — rewriting tempo detection
    return beatTracker.getCurrentBeat();
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeGetChord(
        JNIEnv* env, jobject) {
    return env->NewStringUTF(chordDetector.getChord().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeGetChordTiming(
        JNIEnv* env, jobject) {
    return env->NewStringUTF("");
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeGetChordTones(
        JNIEnv* env, jobject) {
    return env->NewStringUTF(stylePlayer.getChordTonesString().c_str());
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeInitRhythmEngine(
        JNIEnv* env, jobject, jstring wavDir) {
    const char* dir = env->GetStringUTFChars(wavDir, nullptr);
    bool ok = rhythmEngine.loadSamples(dir);
    if (ok) rhythmEngine.loadSlapSamples(dir);  // slap采样加载失败不影响主流程
    env->ReleaseStringUTFChars(wavDir, dir);
    if (ok) cajon.setEnabled(true);
    return ok ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeSetCajonEnergy(
        JNIEnv*, jobject, jfloat energy) { cajon.setEnergy(energy); }

extern "C" JNIEXPORT jfloat JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeGetCajonEnergy(
        JNIEnv*, jobject) { return cajon.getEnergy(); }

extern "C" JNIEXPORT void JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeSetMinCajonEnergy(
        JNIEnv*, jobject, jfloat v) {
    g_minCajonEnergy = (v < 0 ? 0 : (v > 1.0f ? 1.0f : v));
}

extern "C" JNIEXPORT jfloat JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeGetMinCajonEnergy(
        JNIEnv*, jobject) { return g_minCajonEnergy; }

extern "C" JNIEXPORT void JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeSetRhythmGain(
        JNIEnv*, jobject, jfloat gain) {
    if (g_rhythmEngine) g_rhythmEngine->setMasterGain(gain);
}

extern "C" JNIEXPORT jfloat JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeGetRhythmGain(
        JNIEnv*, jobject) {
    return g_rhythmEngine ? g_rhythmEngine->getMasterGain() : 4.8f;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeCajonTick(
        JNIEnv*, jobject) {
    return cajon.getCurrentStep();
}

extern "C" JNIEXPORT jfloatArray JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeGetCajonWeights(
        JNIEnv* env, jobject) {
    jfloatArray arr = env->NewFloatArray(32);
    env->SetFloatArrayRegion(arr, 0, 32, cajon.getStepWeights());
    return arr;
}

extern "C" JNIEXPORT void JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeSetAssistType(
        JNIEnv*, jobject, jint v) {
    const int mode = v == 1 ? 1 : 0;
    assistType.store(mode, std::memory_order_relaxed);
    const bool drumMode = mode == 1;
    g_bassLoopMode.store(drumMode, std::memory_order_relaxed);
    cajon.setEnabled(!drumMode);
    drumLoopEngine.setBassLoopEnabled(drumMode);
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeGetDebugInfo(
        JNIEnv* env, jobject) {
    return env->NewStringUTF(stylePlayer.getDebugInfo().c_str());
}

extern "C" JNIEXPORT void JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeResetChord(
        JNIEnv*, jobject) {
    {
        std::lock_guard<std::mutex> stateLock(g_bassChordPushMutex);
        chordDetector.detect(std::set<int>());
        g_chordDisplayStr = "-";
        g_previousLowNotes = midi.getNoteSetSnapshot().lowNotes;
        g_previousLowNotesValid = true;
        g_bassChordPushDeferred = false;
        drumLoopEngine.setBassChordContext(-1, -1, {}, beatTracker.getCurrentBpm());
    }
    stylePlayer.setChordRoot(60, "major");
}

extern "C" JNIEXPORT void JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeUpdateChordRoot(
        JNIEnv* env, jobject, jint root, jstring chordName) {
    const char* name = env->GetStringUTFChars(chordName, nullptr);
    stylePlayer.setChordRoot(root, name);
    env->ReleaseStringUTFChars(chordName, name);
}

extern "C" JNIEXPORT void JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeSetAccompVolume(
        JNIEnv*, jobject, jdouble vol) {
    audio.enqueueSetGain(1, vol);
}

extern "C" JNIEXPORT void JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeSetLeadVolume(
        JNIEnv*, jobject, jdouble vol) {
    audio.enqueueSetGain(0, vol);
}

extern "C" JNIEXPORT jdouble JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeGetAccompGain(
        JNIEnv*, jobject) { return audio.getGain(1); }

extern "C" JNIEXPORT jdouble JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeGetLeadGain(
        JNIEnv*, jobject) { return audio.getGain(0); }

extern "C" JNIEXPORT jboolean JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeDumpStyleDebug(
        JNIEnv* env, jobject, jstring styPath, jstring outputPath) {
    const char* sty = env->GetStringUTFChars(styPath, nullptr);
    const char* out = env->GetStringUTFChars(outputPath, nullptr);
    bool ok = stylePlayer.dumpDebug(sty, out);
    env->ReleaseStringUTFChars(styPath, sty);
    env->ReleaseStringUTFChars(outputPath, out);
    return ok ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeLoadStyleSoundFont(
        JNIEnv* env, jobject, jstring sf2Path) {
    const char* path = env->GetStringUTFChars(sf2Path, nullptr);
    audio.loadSoundFont(1, path); // load sf2 onto accompaniment synth
    env->ReleaseStringUTFChars(sf2Path, path);
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeGetStyleChannels(
        JNIEnv* env, jobject) {
    return env->NewStringUTF(stylePlayer.getChannelsJson().c_str());
}

extern "C" JNIEXPORT void JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeSetStyleChannelInst(
        JNIEnv*, jobject, jint channel, jint bank, jint program) {
    stylePlayer.setChannelOverride(channel, bank, program);
    audio.enqueueProgramChange(1, (int)channel, (int)bank, (int)program);
    audio.enqueueAllNotesOff(1, (int)channel);
}

extern "C" JNIEXPORT void JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeToggleMute(
        JNIEnv*, jobject, jint channel) {
    stylePlayer.toggleMute(channel);
    if (stylePlayer.isMuted(channel)) audio.enqueueAllNotesOff(1, (int)channel);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeGetActiveChannels(
        JNIEnv*, jobject) {
    return (jint)stylePlayer.getActiveChannels();
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeIsChannelMuted(
        JNIEnv*, jobject, jint channel) {
    return stylePlayer.isMuted((int)channel) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeSetHumanize(
        JNIEnv*, jobject, jfloat timing, jfloat velocity) {
    stylePlayer.setHumanize(timing, velocity);
}

extern "C" JNIEXPORT jfloat JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeGetHumanizeTiming(
        JNIEnv*, jobject) { return stylePlayer.getHumanizeTiming(); }

extern "C" JNIEXPORT jfloat JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeGetHumanizeVelocity(
        JNIEnv*, jobject) { return stylePlayer.getHumanizeVelocity(); }

extern "C" JNIEXPORT void JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeSetReverb(
        JNIEnv*, jobject, jdouble roomSize, jdouble level) {
    audio.enqueueSetReverb(0, roomSize, level);
    audio.enqueueSetReverb(1, roomSize, level);
    if (g_rhythmEngine) g_rhythmEngine->setReverb((float)roomSize, (float)level);
}

extern "C" JNIEXPORT jdouble JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeGetReverbRoomSize(
        JNIEnv*, jobject) { return audio.getReverbRoomSize(1); }

extern "C" JNIEXPORT jdouble JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeGetReverbLevel(
        JNIEnv*, jobject) { return audio.getReverbLevel(1); }

// ===== Drum Loops (opus → 流式拉伸 → 循环播放) =====

extern "C" JNIEXPORT jboolean JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeDrumLoopRenderStart(
        JNIEnv* env, jobject, jstring name, jint sampleRate, jfloat ratio, jint gen,
        jdouble logicalOriginSec, jdouble logicalDurationSec) {
    const char* n = env->GetStringUTFChars(name, nullptr);
    bool ok = drumLoopEngine.renderStart(n, sampleRate, ratio, gen,
                                         logicalOriginSec, logicalDurationSec);
    env->ReleaseStringUTFChars(name, n);
    return ok ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeDrumLoopRenderCommit(
        JNIEnv* env, jobject, jstring name, jint gen, jint state) {
    const char* n = env->GetStringUTFChars(name, nullptr);
    bool ok = drumLoopEngine.renderCommit(n, gen, state);
    env->ReleaseStringUTFChars(name, n);
    return ok ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeDrumLoopRenderFeed(
        JNIEnv* env, jobject, jstring name, jshortArray pcm, jint gen) {
    const char* n = env->GetStringUTFChars(name, nullptr);
    jsize len = env->GetArrayLength(pcm);
    jshort* elems = env->GetShortArrayElements(pcm, nullptr);
    int64_t progress = drumLoopEngine.renderFeed(n, (const int16_t*)elems, (int32_t)len, gen);
    env->ReleaseShortArrayElements(pcm, elems, JNI_ABORT);
    env->ReleaseStringUTFChars(name, n);
    return progress;
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeDrumLoopRenderFinishStep(
        JNIEnv* env, jobject, jstring name, jint gen) {
    const char* n = env->GetStringUTFChars(name, nullptr);
    int64_t r = drumLoopEngine.renderFinishStep(n, gen);
    env->ReleaseStringUTFChars(name, n);
    return r;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeDrumLoopPromotePending(
        JNIEnv* env, jobject, jstring name) {
    const char* n = env->GetStringUTFChars(name, nullptr);
    bool ok = drumLoopEngine.promotePending(n);
    env->ReleaseStringUTFChars(name, n);
    return ok ? JNI_TRUE : JNI_FALSE;
}

// ===== 鼓循环: 播放控制 (beat-0 切换 / 淡出 / 延迟测量) =====

extern "C" JNIEXPORT void JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeDrumLoopRequestSwitch(
        JNIEnv* env, jobject, jstring name) {
    const char* n = env->GetStringUTFChars(name, nullptr);
    std::lock_guard<std::mutex> lk(g_drumSwitchMutex);
    g_pendingDrumSwitch = n;
    env->ReleaseStringUTFChars(name, n);
}

extern "C" JNIEXPORT void JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeDrumLoopFadeOut(
        JNIEnv*, jobject, jint durationMs) {
    drumLoopEngine.fadeOut(durationMs);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeDrumLoopIsPlaying(
        JNIEnv*, jobject) {
    return drumLoopEngine.isPlaying() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeDrumLoopGetAndClearSwitchEvent(
        JNIEnv* env, jobject) {
    if (g_drumSwitchEvent.exchange(0)) {
        std::lock_guard<std::mutex> lk(g_drumSwitchMutex);
        return env->NewStringUTF(g_drumSwitchEventName.c_str());
    }
    return env->NewStringUTF("");
}

extern "C" JNIEXPORT jdouble JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeDrumLoopGetAndClearLatencyMs(
        JNIEnv*, jobject) {
    return (jdouble)drumLoopEngine.getAndClearLatencyMs();
}

extern "C" JNIEXPORT jdouble JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeDrumLoopGetAndClearBpmPrep(
        JNIEnv*, jobject) {
    // native 来源（×2/÷2、CC90）的预备变速事件: 返回目标 BPM; -1 = 无。
    // 这里只领取事件；Kotlin 真正启动预备流程后再调用 nativeBpmPrepStarted() 确认。
    std::lock_guard<std::mutex> lk(g_bpmPrepMutex);
    if (g_bpmPrepEvent.exchange(0)) {
        return (jdouble)g_bpmPrepTarget.load();
    }
    return -1.0;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeBpmPrepStarted(
        JNIEnv*, jobject, jdouble target) {
    std::lock_guard<std::mutex> lk(g_bpmPrepMutex);
    const double latestTarget = g_bpmPrepTarget.load();
    if (std::abs(latestTarget - (double)target) >= 0.001) {
        __android_log_print(ANDROID_LOG_INFO, "BeatTracker",
                            "Ignore stale BPM prep ack: %.1f (latest %.1f)",
                            (double)target, latestTarget);
        return JNI_FALSE;
    }

    // 长按可能先越过当前速度、随后又回到原值。此时 Kotlin 会取消推测渲染；
    // native 也必须清掉目标并发布一次 UI 更新，否则黄色“待生效”状态会永久保留。
    if (std::abs(latestTarget - beatTracker.getCurrentBpm()) < 0.001) {
        g_bpmPrepTarget.store(-1.0);
        g_bpmPrepPending.store(0);
        g_bpmPrepEvent.store(0);
        g_bpmPrepDispatched = false;
        g_pendingBpmUpdate.store(latestTarget);
        return JNI_TRUE;
    }

    // 防止直接派发的 +/- 与尚在队列中的轮询事件重复处理同一目标。
    g_bpmPrepEvent.store(0);
    if (g_bpmPrepPending.load()) {
        // 以渲染流程实际启动的时刻再次检查最后 1/8 安全窗。
        g_bpmPrepEarliestMeasure = std::max(
            g_bpmPrepEarliestMeasure, calculateBpmPrepEarliestMeasureLocked());
        g_bpmPrepDispatched = true;
    }
    __android_log_print(ANDROID_LOG_INFO, "BeatTracker",
                        "BPM prep started: %.1f pending=%d earliest=%llu",
                        latestTarget, g_bpmPrepPending.load(),
                        (unsigned long long)g_bpmPrepEarliestMeasure);
    return JNI_TRUE;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeDrumLoopGetAndClearStoppedEvent(
        JNIEnv*, jobject) {
    return drumLoopEngine.getAndClearStoppedEvent();
}

extern "C" JNIEXPORT void JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeDrumLoopSyncBeat(
        JNIEnv*, jobject) {
    // 鼓循环模式: 同步节拍器到第一拍 (播放开始时调用, 让节拍点从第一拍亮起)
    beatTracker.sync();
}

extern "C" JNIEXPORT void JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeDrumLoopRenderCancelAll(
        JNIEnv*, jobject) {
    drumLoopEngine.renderCancelAll();
}

// ===== Drum Loops: 低质量低延迟 (Signalsmith) =====

extern "C" JNIEXPORT jboolean JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeDrumLoopSgsmStart(
        JNIEnv* env, jobject, jstring name, jint sampleRate, jfloat ratio, jint gen,
        jdouble logicalOriginSec, jdouble logicalDurationSec) {
    const char* n = env->GetStringUTFChars(name, nullptr);
    bool ok = drumLoopEngine.sgsmStart(n, sampleRate, ratio, gen,
                                       logicalOriginSec, logicalDurationSec);
    env->ReleaseStringUTFChars(name, n);
    return ok ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jdouble JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeDrumLoopGetPlaybackPosition(
        JNIEnv* env, jobject, jstring name) {
    const char* n = env->GetStringUTFChars(name, nullptr);
    double pos = drumLoopEngine.getPlaybackPositionSec(n);
    env->ReleaseStringUTFChars(name, n);
    return pos;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeDrumLoopArmTempoCut(
        JNIEnv* env, jobject, jstring name, jint gen, jdouble delaySec,
        jdoubleArray targetOffsetsSec, jdoubleArray nextDelaysSec, jdouble targetBpm) {
    if (!targetOffsetsSec || !nextDelaysSec) return JNI_FALSE;
    const jsize offsetCount = env->GetArrayLength(targetOffsetsSec);
    const jsize delayCount = env->GetArrayLength(nextDelaysSec);
    if (offsetCount <= 0 || offsetCount != delayCount) return JNI_FALSE;
    std::vector<double> offsets((size_t)offsetCount);
    std::vector<double> nextDelays((size_t)delayCount);
    env->GetDoubleArrayRegion(targetOffsetsSec, 0, offsetCount, offsets.data());
    env->GetDoubleArrayRegion(nextDelaysSec, 0, delayCount, nextDelays.data());
    if (env->ExceptionCheck()) return JNI_FALSE;
    const char* n = env->GetStringUTFChars(name, nullptr);
    bool ok = drumLoopEngine.armTempoCut(
        n, gen, delaySec, offsets, nextDelays, targetBpm);
    env->ReleaseStringUTFChars(name, n);
    return ok ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeDrumLoopCancelTempoCut(
        JNIEnv*, jobject) {
    drumLoopEngine.cancelTempoCut();
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeDrumLoopArmAutoFillJump(
        JNIEnv* env, jobject, jstring name, jint gen, jdouble delaySec,
        jdouble destinationLogicalSec) {
    const char* n = env->GetStringUTFChars(name, nullptr);
    bool ok = drumLoopEngine.armAutoFillJump(
        n, gen, delaySec, destinationLogicalSec);
    env->ReleaseStringUTFChars(name, n);
    return ok ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeDrumLoopCancelAutoFillJump(
        JNIEnv*, jobject) {
    drumLoopEngine.cancelAutoFillJump();
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeDrumLoopStartOneShot(
        JNIEnv* env, jobject, jstring name, jdouble logicalStartSec, jint fadeMs,
        jstring nextName, jboolean stopAfter, jdouble delaySec, jdouble endFadeSec) {
    const char* n = env->GetStringUTFChars(name, nullptr);
    const char* next = env->GetStringUTFChars(nextName, nullptr);
    const bool ok = drumLoopEngine.startOneShot(
        n, logicalStartSec, fadeMs, next, stopAfter == JNI_TRUE, delaySec, endFadeSec);
    env->ReleaseStringUTFChars(nextName, next);
    env->ReleaseStringUTFChars(name, n);
    return ok ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeDrumLoopGetAndClearOneShotStartedEvent(
        JNIEnv* env, jobject) {
    const std::string name = drumLoopEngine.getAndClearOneShotStartedEvent();
    return env->NewStringUTF(name.c_str());
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeDrumLoopUpdateOneShotNext(
        JNIEnv* env, jobject, jstring nextName) {
    const char* next = env->GetStringUTFChars(nextName, nullptr);
    const bool ok = drumLoopEngine.updateOneShotNext(next);
    env->ReleaseStringUTFChars(nextName, next);
    return ok ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeDrumLoopCancelOneShot(
        JNIEnv*, jobject) {
    drumLoopEngine.cancelOneShot();
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeDrumLoopSetBassMidi(
        JNIEnv* env, jobject, jstring name, jdoubleArray phases,
        jintArray velocities, jintArray downbeats, jintArray strongBeats,
        jintArray noteKinds, jintArray phraseGroups) {
    if (!name || !phases || !velocities || !downbeats || !strongBeats ||
        !noteKinds || !phraseGroups) return JNI_FALSE;
    const jsize phaseCount = env->GetArrayLength(phases);
    const jsize velocityCount = env->GetArrayLength(velocities);
    const jsize downbeatCount = env->GetArrayLength(downbeats);
    const jsize strongBeatCount = env->GetArrayLength(strongBeats);
    const jsize kindCount = env->GetArrayLength(noteKinds);
    const jsize groupCount = env->GetArrayLength(phraseGroups);
    if (phaseCount != velocityCount || phaseCount != downbeatCount ||
        phaseCount != strongBeatCount || phaseCount != kindCount ||
        phaseCount != groupCount) return JNI_FALSE;
    std::vector<double> phaseValues((size_t)phaseCount);
    std::vector<int> velocityValues((size_t)velocityCount);
    std::vector<int> downbeatValues((size_t)downbeatCount);
    std::vector<int> strongBeatValues((size_t)strongBeatCount);
    std::vector<int> kindValues((size_t)kindCount);
    std::vector<int> groupValues((size_t)groupCount);
    if (phaseCount > 0) {
        env->GetDoubleArrayRegion(phases, 0, phaseCount, phaseValues.data());
        std::vector<jint> rawVelocities((size_t)velocityCount);
        std::vector<jint> rawDownbeats((size_t)downbeatCount);
        std::vector<jint> rawStrongBeats((size_t)strongBeatCount);
        std::vector<jint> rawKinds((size_t)kindCount);
        std::vector<jint> rawGroups((size_t)groupCount);
        env->GetIntArrayRegion(velocities, 0, velocityCount, rawVelocities.data());
        env->GetIntArrayRegion(downbeats, 0, downbeatCount, rawDownbeats.data());
        env->GetIntArrayRegion(strongBeats, 0, strongBeatCount, rawStrongBeats.data());
        env->GetIntArrayRegion(noteKinds, 0, kindCount, rawKinds.data());
        env->GetIntArrayRegion(phraseGroups, 0, groupCount, rawGroups.data());
        if (env->ExceptionCheck()) return JNI_FALSE;
        std::transform(rawVelocities.begin(), rawVelocities.end(), velocityValues.begin(),
                       [](jint value) { return (int)value; });
        std::transform(rawDownbeats.begin(), rawDownbeats.end(), downbeatValues.begin(),
                       [](jint value) { return (int)value; });
        std::transform(rawStrongBeats.begin(), rawStrongBeats.end(),
                       strongBeatValues.begin(),
                       [](jint value) { return (int)value; });
        std::transform(rawKinds.begin(), rawKinds.end(), kindValues.begin(),
                       [](jint value) { return (int)value; });
        std::transform(rawGroups.begin(), rawGroups.end(), groupValues.begin(),
                       [](jint value) { return (int)value; });
    }
    const char* n = env->GetStringUTFChars(name, nullptr);
    if (!n) return JNI_FALSE;
    const bool ok = drumLoopEngine.setBassMidiEvents(
        n, phaseValues, velocityValues, downbeatValues, strongBeatValues,
        kindValues, groupValues);
    env->ReleaseStringUTFChars(name, n);
    return ok ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeDrumLoopGetAndClearOneShotSwitchEvent(
        JNIEnv* env, jobject) {
    const std::string name = drumLoopEngine.getAndClearOneShotSwitchEvent();
    return env->NewStringUTF(name.c_str());
}

extern "C" JNIEXPORT jdouble JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeDrumLoopGetAndClearTempoCutEvent(
        JNIEnv*, jobject) {
    const double bpm = drumLoopEngine.getAndClearTempoCutEvent();
    if (bpm > 0.0) stylePlayer.setTempoBPM(bpm);
    return bpm;
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeDrumLoopSgsmFeed(
        JNIEnv* env, jobject, jstring name, jshortArray pcm, jint gen) {
    const char* n = env->GetStringUTFChars(name, nullptr);
    jsize len = env->GetArrayLength(pcm);
    jshort* elems = env->GetShortArrayElements(pcm, nullptr);
    int64_t progress = drumLoopEngine.sgsmFeed(n, (const int16_t*)elems, (int32_t)len, gen);
    env->ReleaseShortArrayElements(pcm, elems, JNI_ABORT);
    env->ReleaseStringUTFChars(name, n);
    return progress;
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeDrumLoopSgsmFinishStep(
        JNIEnv* env, jobject, jstring name, jint gen) {
    const char* n = env->GetStringUTFChars(name, nullptr);
    int64_t r = drumLoopEngine.sgsmFinishStep(n, gen);
    env->ReleaseStringUTFChars(name, n);
    return r;
}

extern "C" JNIEXPORT void JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeDrumLoopPlay(
        JNIEnv* env, jobject, jstring name) {
    const char* n = env->GetStringUTFChars(name, nullptr);
    drumLoopEngine.play(n);
    env->ReleaseStringUTFChars(name, n);
}

extern "C" JNIEXPORT void JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeDrumLoopPlayAt(
        JNIEnv* env, jobject, jstring name, jdouble logicalStartSec) {
    const char* n = env->GetStringUTFChars(name, nullptr);
    drumLoopEngine.playAt(n, logicalStartSec);
    env->ReleaseStringUTFChars(name, n);
}

extern "C" JNIEXPORT void JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeDrumLoopStop(
        JNIEnv*, jobject) {
    drumLoopEngine.stop();
}

extern "C" JNIEXPORT void JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeDrumLoopClear(
        JNIEnv*, jobject) {
    drumLoopEngine.clear();
}

extern "C" JNIEXPORT void JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeDrumLoopSetVolume(
        JNIEnv*, jobject, jfloat volume) {
    drumLoopEngine.setVolume(volume);
}

extern "C" JNIEXPORT void JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeDrumLoopSetRate(
        JNIEnv*, jobject, jfloat rate) {
    drumLoopEngine.setRate(rate);
}
