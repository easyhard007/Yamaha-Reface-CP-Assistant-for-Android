#pragma once

#include <oboe/Oboe.h>
#include <mutex>
#include <memory>
#include <vector>
#include <string>
#include "fluidlite.h"

class AudioEngine : public oboe::AudioStreamCallback {
public:
    AudioEngine() = default;
    ~AudioEngine();

    // 初始化：同时创建 Lead 和 Accomp 两个合成器
    // sf2Path: 默认加载给 Lead 的音色路径
    bool init(const char* sf2Path);

    void start();
    void stop();

    // >>>>> 核心修改：增加 target 参数 (0=Lead, 1=Accomp) >>>>>

    // MIDI 控制
    void playNote(int target, int note, int velocity);
    void stopNote(int target, int note);
    void sendMidiControlChange(int target, int controller, int value);

    // 乐器管理
    bool loadSoundFont(int target, const char* path);
    int getInstrumentCount(int target);
    const char* getInstrumentName(int target, int index);
    void setInstrument(int target, int index);

    // 音量控制
    void setMasterVolume(int target, float gain);

    // <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<

    // Oboe 回调
    oboe::DataCallbackResult onAudioReady(
            oboe::AudioStream *audioStream,
            void *audioData,
            int32_t numFrames) override;

private:
    struct InstrumentInfo {
        std::string name;
        int bank;
        int program;
    };

    // 辅助函数：扫描指定 Synth 的乐器到指定列表
    void scanPresets(fluid_synth_t* synth, std::vector<InstrumentInfo>& list);

    std::shared_ptr<oboe::AudioStream> stream;
    std::mutex mLock;

    fluid_settings_t* mSettings = nullptr; // 配置可以共用一个

    // >>>>> 明确的两个变量，不搞数组 >>>>>
    fluid_synth_t* mLeadSynth = nullptr;   // 主旋律 (键盘)
    fluid_synth_t* mAccompSynth = nullptr; // 伴奏 (自动)

    int mLeadSoundFontId = -1;
    int mAccompSoundFontId = -1;

    // 两个乐器列表缓存
    std::vector<InstrumentInfo> mLeadInstruments;
    std::vector<InstrumentInfo> mAccompInstruments;

    // 混音缓冲区
    std::vector<float> mMixBuffer;
    // <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<
};