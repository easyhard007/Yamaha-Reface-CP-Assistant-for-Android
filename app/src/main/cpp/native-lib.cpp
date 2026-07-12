#include <jni.h>
#include <string>
#include "AudioEngine.h"

static AudioEngine engine;

extern "C" JNIEXPORT void JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeInit(
        JNIEnv* env,
jobject /* this */,
jstring sf2Path) {
const char *path = env->GetStringUTFChars(sf2Path, nullptr);
engine.init(path);
engine.start(); // 初始化完成后直接启动音频流
env->ReleaseStringUTFChars(sf2Path, path);
}

extern "C" JNIEXPORT void JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeNoteOn(
        JNIEnv* env, jobject, jint note, jint velocity) {
    // 指定 target = 0 (Lead Synth)
    engine.playNote(0, note, velocity);
}
extern "C" JNIEXPORT void JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeNoteOff(
        JNIEnv* env,
jobject /* this */,
jint note) {
engine.stopNote(0,note);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeGetInstrumentCount(
        JNIEnv* env, jobject) {
    return engine.getInstrumentCount(0);
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeGetInstrumentName(
        JNIEnv* env, jobject, jint index) {
    const char* name = engine.getInstrumentName(0,index);
    // 把 C 字符串转为 Java 字符串
    return env->NewStringUTF(name ? name : "Unknown");
}

extern "C" JNIEXPORT void JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeSetInstrument(
        JNIEnv* env, jobject, jint index) {
    engine.setInstrument(0,index);
}

extern "C" JNIEXPORT void JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeSetMasterVolume(
        JNIEnv* env, jobject, jfloat gain) {
    engine.setMasterVolume(0,gain);
}

extern "C" JNIEXPORT void JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeMidiControlChange(
        JNIEnv* env, jobject, jint controller, jint value) {
    engine.sendMidiControlChange(0,controller, value);
}

extern "C" JNIEXPORT void JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeLoadSoundFont(
        JNIEnv* env, jobject, jstring sf2Path) {
    const char *path = env->GetStringUTFChars(sf2Path, nullptr);
    engine.loadSoundFont(0,path);
    env->ReleaseStringUTFChars(sf2Path, path);
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_chenyinan_reface_1cp_1assist_MainActivity_nativeGetChordInfo(
        JNIEnv* env, jobject) {
    std::string info = engine.getChordInfo();
    return env->NewStringUTF(info.c_str());
}

