#include <jni.h>
#include <string>
#include "AudioEngine.h"
#include "MidiProcessor.h"
#include "StylePlayer.h"

static AudioEngine audio;
static MidiProcessor midi;
static StylePlayer stylePlayer;

// ===== Audio Engine (FluidLite) =====
extern "C" JNIEXPORT void JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeInit(
        JNIEnv* env, jobject, jstring sf2Path) {
    const char *path = env->GetStringUTFChars(sf2Path, nullptr);
    audio.init(path);
    audio.start();
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
    auto r = midi.processNoteOn(note, velocity);
    return encodeResult(r.sustainCCToSend, r.bassNote, r.bassVelocity);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeProcessNoteOff(
        JNIEnv*, jobject, jint note) {
    auto r = midi.processNoteOff(note);
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
    if (!ok) {
        return env->NewStringUTF("{\"error\":\"Failed to load style\"}");
    }
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
    stylePlayer.start(&audio, 1);
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
    return (jint)stylePlayer.getCurrentBeat();
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
