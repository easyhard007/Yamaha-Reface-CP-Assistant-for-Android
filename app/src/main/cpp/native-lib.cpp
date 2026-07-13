#include <jni.h>
#include <string>
#include "AudioEngine.h"
#include "MidiProcessor.h"

static AudioEngine audio;
static MidiProcessor midi;

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
    audio.playNote(0, note, velocity);
}

extern "C" JNIEXPORT void JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeNoteOff(
        JNIEnv*, jobject, jint note) {
    audio.stopNote(0, note);
}

extern "C" JNIEXPORT void JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeSendCC(
        JNIEnv*, jobject, jint controller, jint value) {
    audio.sendCC(0, controller, value);
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
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeChangeSplitPoint(
        JNIEnv*, jobject, jint delta) { return midi.changeSplitPoint(delta); }
