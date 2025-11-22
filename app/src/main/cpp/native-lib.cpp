#include <jni.h>
#include <string>
#include "AudioEngine.h"

static AudioEngine engine;

extern "C" JNIEXPORT void JNICALL
Java_com_example_cynarranger_MainActivity_nativeInit(
        JNIEnv* env,
jobject /* this */,
jstring sf2Path) {
const char *path = env->GetStringUTFChars(sf2Path, nullptr);
engine.init(path);
engine.start(); // 初始化完成后直接启动音频流
env->ReleaseStringUTFChars(sf2Path, path);
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_cynarranger_MainActivity_nativeNoteOn(
        JNIEnv* env,
jobject /* this */,
jint note,
        jint velocity) {
engine.playNote(note, velocity);
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_cynarranger_MainActivity_nativeNoteOff(
        JNIEnv* env,
jobject /* this */,
jint note) {
engine.stopNote(note);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_example_cynarranger_MainActivity_nativeGetInstrumentCount(
        JNIEnv* env, jobject) {
    return engine.getInstrumentCount();
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_example_cynarranger_MainActivity_nativeGetInstrumentName(
        JNIEnv* env, jobject, jint index) {
    const char* name = engine.getInstrumentName(index);
    // 把 C 字符串转为 Java 字符串
    return env->NewStringUTF(name ? name : "Unknown");
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_cynarranger_MainActivity_nativeSetInstrument(
        JNIEnv* env, jobject, jint index) {
    engine.setInstrument(index);
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_cynarranger_MainActivity_nativeMidiControlChange(
        JNIEnv* env, jobject, jint controller, jint value) {
    engine.sendMidiControlChange(controller, value);
}