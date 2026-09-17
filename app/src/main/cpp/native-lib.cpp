#include <jni.h>
#include <string>
#include <algorithm>
#include <cmath>
#include <mutex>
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
float g_bassVolume = 0.8f;  // AudioEngine 混音用
static int assistType = 0;
float g_minCajonEnergy = 0.0f;
std::atomic<double> g_pendingBpmUpdate{-1.0};
std::atomic<double> g_pendingRhythmGainUpdate{-1.0};
std::atomic<double> g_pendingMinEnergyUpdate{-1.0};
static std::atomic<int> g_pendingTempoHighlight{0};
static std::atomic<int> g_pendingScatterUpdate{0};
static std::atomic<int> g_pendingTempoFlash{0};

// 相位调试
static std::atomic<double> g_measureStartMs{0};
static bool g_syncJustTriggered = false;
static std::atomic<int> g_pendingChordUpdate{0};
static std::atomic<float> g_pendingBassVolUpdate{-1.0f};
static std::atomic<int> g_pendingBpmMult{0};  // 2=×2, -2=÷2, 0=无
static std::string g_chordDisplayStr;
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
    // ×2/÷2 请求: 在小节第一拍时先重设 tempo 再触发第一拍
    if (step == 0) {
        int mult = g_pendingBpmMult.exchange(0);
        if (mult == 2 || mult == -2) {
            double cur = beatTracker.getCurrentBpm();
            double newBpm = (mult == 2) ? cur * 2.0 : std::ceil(cur / 2.0);
            beatTracker.setTempo(newBpm);
            g_pendingBpmUpdate.store(newBpm);
            __android_log_print(ANDROID_LOG_INFO, "BeatTracker",
                "BPM mult %d: %.1f → %.1f", mult, cur, newBpm);
        }
    }
    int bassVel = cajon.processStep(step, bpm);
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    double ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();

    float autoEnergy = cajon.updateEnergy(ms, beatTracker.getCurrentBpm());
    float energy = std::max(autoEnergy, g_minCajonEnergy);
    cajon.setEnergy(energy);

    // 贝斯助手编排
    bassAssist.processStep(step, bassVel, cajon.getLastToneVel(), ms, bpm,
                            g_chordDisplayStr, chordDetector.getChordNotes(),
                            energy, audio, g_rhythmEngine);
    pushEnergyDisplayUpdate();
    // 每拍首子步推送白点更新
    if (step % 8 == 0) pushBeatDotUpdate();
    // beat 0 开始时重置计时器
    if (step % 32 == 0) g_measureStartMs.store(ms);
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
        if (assistType == 0 && g_rhythmEngine) {
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
            if (newBpm < 30.0) newBpm = 30.0;
            if (newBpm > 300.0) newBpm = 300.0;
            beatTracker.setTempo(newBpm);
            stylePlayer.setTempoBPM(newBpm);
            g_pendingBpmUpdate.store(newBpm);
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
    // 新和弦识别: 用 AllActiveNotes
    chordDetector.detect(midi.getAllNotes());
    if (chordDetector.changed()) {
        g_chordDisplayStr = chordDetector.getChord();
        g_pendingChordUpdate.store(1);
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
    chordDetector.detect(midi.getAllNotes());
    if (chordDetector.changed()) {
        g_chordDisplayStr = chordDetector.getChord();
        g_pendingChordUpdate.store(1);
    }
    return encodeResult(r.sustainCCToSend, r.bassNote, 0);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeProcessCC(
        JNIEnv*, jobject, jint controller, jint value) {
    auto r = midi.processCC(controller, value);
    float bv = midi.getAndClearBassVolumeFromCC();
    if (bv >= 0.0f) {
        g_bassVolume = bv;
        bassAssist.setVolume(bv);
        g_pendingBassVolUpdate.store(bv);
    }
    return r.sustainCCToSend;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeSetAutoSustain(
        JNIEnv*, jobject, jboolean enabled) {
    int cc = midi.setAutoSustainEnabled(enabled);
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
        JNIEnv*, jobject, jint delta) { return midi.changeSplitPoint(delta); }

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
        JNIEnv*, jobject, jfloat volume) { bassAssist.setVolume(volume); g_bassVolume = volume; }
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
        beatTracker.tapTempo();
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeRequestBpmMult(
        JNIEnv*, jobject, jint mult) {
    g_pendingBpmMult.store(mult);
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
        JNIEnv*, jobject, jint v) { assistType = v; cajon.setEnabled(v == 0); }

extern "C" JNIEXPORT jstring JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeGetDebugInfo(
        JNIEnv* env, jobject) {
    return env->NewStringUTF(stylePlayer.getDebugInfo().c_str());
}

extern "C" JNIEXPORT void JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeResetChord(
        JNIEnv*, jobject) {
    chordDetector.detect(std::set<int>());
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
        JNIEnv* env, jobject, jstring name, jint sampleRate, jfloat ratio, jint gen) {
    const char* n = env->GetStringUTFChars(name, nullptr);
    bool ok = drumLoopEngine.renderStart(n, sampleRate, ratio, gen);
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

extern "C" JNIEXPORT jboolean JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeDrumLoopRenderFinish(
        JNIEnv* env, jobject, jstring name, jint gen) {
    const char* n = env->GetStringUTFChars(name, nullptr);
    bool ok = drumLoopEngine.renderFinish(n, gen);
    env->ReleaseStringUTFChars(name, n);
    return ok ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeDrumLoopRenderCancelAll(
        JNIEnv*, jobject) {
    drumLoopEngine.renderCancelAll();
}

// ===== Drum Loops: 低质量低延迟 (Signalsmith) =====

extern "C" JNIEXPORT jboolean JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeDrumLoopSgsmStart(
        JNIEnv* env, jobject, jstring name, jint sampleRate, jfloat ratio, jint gen) {
    const char* n = env->GetStringUTFChars(name, nullptr);
    bool ok = drumLoopEngine.sgsmStart(n, sampleRate, ratio, gen);
    env->ReleaseStringUTFChars(name, n);
    return ok ? JNI_TRUE : JNI_FALSE;
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

extern "C" JNIEXPORT jboolean JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeDrumLoopSgsmFinish(
        JNIEnv* env, jobject, jstring name, jint gen) {
    const char* n = env->GetStringUTFChars(name, nullptr);
    bool ok = drumLoopEngine.sgsmFinish(n, gen);
    env->ReleaseStringUTFChars(name, n);
    return ok ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeDrumLoopPlay(
        JNIEnv* env, jobject, jstring name) {
    const char* n = env->GetStringUTFChars(name, nullptr);
    drumLoopEngine.play(n);
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
