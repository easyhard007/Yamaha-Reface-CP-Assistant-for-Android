#pragma once

#include <oboe/Oboe.h>
#include <mutex>
#include <memory>
#include <vector> // 引入 vector
#include <string> // 引入 string
// 引入 FluidLite 主头文件
// 因为我们在 CMake 中使用了 add_subdirectory，路径会自动处理，直接引用即可
#include "fluidlite.h"

class AudioEngine : public oboe::AudioStreamCallback {
public:
    AudioEngine() = default;
    ~AudioEngine();

    // 初始化：加载 SoundFont，设置参数
    bool init(const char* sf2Path);

    // 启动/停止 Oboe 音频流
    void start();
    void stop();

    // MIDI 核心功能
    void playNote(int note, int velocity);
    void stopNote(int note);
    void sendMidiControlChange(int controller, int value);


    // 乐器管理
    bool loadSoundFont(const char* path);
    int getInstrumentCount();
    const char* getInstrumentName(int index);
    void setInstrument(int index);

    void setMasterVolume(float gain); // gain: 0.0 - 5.0 (推荐范围)


    // Oboe 音频回调函数
    oboe::DataCallbackResult onAudioReady(
            oboe::AudioStream *audioStream,
            void *audioData,
            int32_t numFrames) override;

private:
    // 辅助函数：扫描当前 SF2 的所有乐器
    void scanPresets();

    std::shared_ptr<oboe::AudioStream> stream;
    std::mutex mLock; // 线程锁，防止 UI 线程和音频线程冲突

    // FluidLite 核心指针
    fluid_settings_t* mSettings = nullptr;
    fluid_synth_t* mSynth = nullptr;
    int mSoundFontId = -1;

    // 定义乐器映射结构
    struct InstrumentInfo {
        std::string name; // 显示给 UI 的名字
        int bank;         // 真实的 Bank 号
        int program;      // 真实的 Program 号
    };

    // 乐器列表缓存
    std::vector<InstrumentInfo> mInstruments;
};