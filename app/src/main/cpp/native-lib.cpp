#include <jni.h>
#include <string>
#include <algorithm>
#include <mutex>
#include "AudioEngine.h"
#include "MidiProcessor.h"
#include "StylePlayer.h"
#include "LowChordDetector.h"
#include "TempoTracker.h"
#include "BeatTracker.h"
#include "RhythmAudioEngine.h"
#include "CajonAssistant.h"

static AudioEngine audio;
static MidiProcessor midi;
static StylePlayer stylePlayer;
static LowChordDetector chordDetector;
static TempoTracker tempoTracker;
static BeatTracker beatTracker;
extern RhythmAudioEngine* g_rhythmEngine;
static RhythmAudioEngine rhythmEngine;
static CajonAssistant cajon;
static int assistType = 0;
static float g_minCajonEnergy = 0.0f;
static std::vector<TempoMidiEvent> tempoEvents;

// ===== Cajon 能量: 根据近 2 秒 MIDI 音符密度自动计算 =====
static std::vector<double> recentNoteTimestamps;
static std::mutex energyMutex;
static void updateCajonEnergy(double nowMs);

static void onBeatStep(int step, double bpm, void*) {
    cajon.onStep(step, bpm);
    // 每拍 (step % 8 == 0) 更新一次能量, 让无输入时自然衰减
    if (step % 8 == 0) {
        auto now = std::chrono::steady_clock::now().time_since_epoch();
        double ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
        updateCajonEnergy(ms);
    }
}

static void updateCajonEnergy(double nowMs) {
    std::lock_guard<std::mutex> lock(energyMutex);
    // 清理超过 2 秒的旧时间戳
    double cutoff = nowMs - 2000.0;
    recentNoteTimestamps.erase(
        std::remove_if(recentNoteTimestamps.begin(), recentNoteTimestamps.end(),
            [cutoff](double t) { return t < cutoff; }),
        recentNoteTimestamps.end());
    // 线性映射: 0 个 → 0.0, 20 个 → 1.0
    float autoEnergy = std::min(1.0f, (float)recentNoteTimestamps.size() / 24.0f);
    float energy = std::max(autoEnergy, g_minCajonEnergy);
    cajon.setEnergy(energy);
}

// ===== Audio Engine (FluidLite) =====
extern "C" JNIEXPORT void JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeInit(
        JNIEnv* env, jobject, jstring sf2Path) {
    const char *path = env->GetStringUTFChars(sf2Path, nullptr);
    srand((unsigned int)std::chrono::steady_clock::now().time_since_epoch().count());
    audio.init(path);
    // Init rhythm engine BEFORE audio starts (callback needs g_rhythmEngine set)
    g_rhythmEngine = &rhythmEngine;
    cajon.init(&rhythmEngine);
    audio.start();
    // Beat tracker
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    double ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    tempoTracker.init(4, 4, 75.0, ms);
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
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeSendCC(
        JNIEnv*, jobject, jint controller, jint value) {
    // CC86 → lead gain (0-6.0), CC87 → accomp gain (0-6.0)
    if (controller == 86) { audio.enqueueSetGain(0, value / 127.0 * 6.0); return; }
    if (controller == 87) { audio.enqueueSetGain(1, value / 127.0 * 6.0); return; }
    audio.enqueueCC(0, 0, controller, value);
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
    // 记录 tempo 事件 (最多 100 条)
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    double ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    tempoEvents.push_back({(uint8_t)note, (uint8_t)velocity, ms});
    if (tempoEvents.size() > 100) tempoEvents.erase(tempoEvents.begin());

    // 统计近 2 秒音符密度 → Cajon 能量
    {
        std::lock_guard<std::mutex> lock(energyMutex);
        recentNoteTimestamps.push_back(ms);
    }
    updateCajonEnergy(ms);

    auto r = midi.processNoteOn(note, velocity);
    chordDetector.feedNotes(midi.getLowNotes(), midi.getAllNotes(), midi.getSplitNote());
    // 和弦根音: 来自 LowChordDetector, 由 feedNotes 内部确定
    std::string chordName = chordDetector.getChord();
    if (chordName != "-") {
        int root = chordDetector.getRootMidi();
        stylePlayer.setChordRoot(root, chordName);
    }
    return encodeResult(r.sustainCCToSend, r.bassNote, r.bassVelocity);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeProcessNoteOff(
        JNIEnv*, jobject, jint note) {
    auto r = midi.processNoteOff(note);
    // 不在 NoteOff 时触发和弦检测, 防止降级
    return encodeResult(r.sustainCCToSend, r.bassNote, 0);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeProcessCC(
        JNIEnv*, jobject, jint controller, jint value) {
    auto r = midi.processCC(controller, value);
    return r.sustainCCToSend;
}

extern "C" JNIEXPORT void JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeSetAutoSustain(
        JNIEnv*, jobject, jboolean enabled) {
    midi.setAutoSustainEnabled(enabled);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeGetPendingSustainCC(
        JNIEnv*, jobject) { return midi.getPendingSustainCC(); }

extern "C" JNIEXPORT jstring JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeGetNoteState(
        JNIEnv* env, jobject) {
    return env->NewStringUTF(midi.getNoteStateJson().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeGetChordInfo(
        JNIEnv* env, jobject) {
    return env->NewStringUTF(midi.getChordInfo().c_str());
}

extern "C" JNIEXPORT void JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeSetBassEnhance(
        JNIEnv*, jobject, jboolean enabled) { midi.setBassEnhanceEnabled(enabled); }

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
    tempoEvents.clear();
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
    if (!tempoEvents.empty()) {
        stylePlayer.getTempoBPM();
        double newBpm = tempoTracker.processEvents(tempoEvents);
        tempoEvents.clear();
        // 动态更新播放速度
        if (std::abs(newBpm - stylePlayer.getTempoBPM()) > 0.5) {
            stylePlayer.setTempoBPM(newBpm);
            beatTracker.setTempo(newBpm);
        }
        return newBpm;
    }
    return stylePlayer.getTempoBPM();
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
    beatTracker.sync();
}

extern "C" JNIEXPORT jint JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeGetBeatIndex(
        JNIEnv*, jobject) {
    return tempoTracker.getCurrentBeatIndex();
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeGetChordNotes(
        JNIEnv* env, jobject) {
    static const char* nn[12] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
    auto notes = midi.getLowNotes();
    std::string s;
    for (int n : notes) {
        if (!s.empty()) s += " ";
        s += nn[n % 12] + std::to_string(n / 12 - 1);
    }
    return env->NewStringUTF(s.c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeGetChord(
        JNIEnv* env, jobject) {
    return env->NewStringUTF(chordDetector.getChord().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeGetChordTiming(
        JNIEnv* env, jobject) {
    return env->NewStringUTF(chordDetector.getTiming().c_str());
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
    chordDetector.reset();
    stylePlayer.setChordRoot(60, "major"); // reset to C
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
}

extern "C" JNIEXPORT jdouble JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeGetReverbRoomSize(
        JNIEnv*, jobject) { return audio.getReverbRoomSize(1); }

extern "C" JNIEXPORT jdouble JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeGetReverbLevel(
        JNIEnv*, jobject) { return audio.getReverbLevel(1); }
