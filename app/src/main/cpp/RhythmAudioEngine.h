#pragma once
#include <vector>
#include <string>
#include <atomic>
#include <mutex>
#include "SimpleReverb.h"

struct WavSample {
    std::vector<float> pcm; // mono, 44100Hz float samples
    int sampleRate = 44100;
};

class RhythmAudioEngine {
public:
    RhythmAudioEngine();
    ~RhythmAudioEngine();

    /// 从 assets 目录加载所有 wav 文件 (mainL, mainR, snare, hihat)
    bool loadSamples(const std::string& wavDir);

    /// 加载 slap 采样 (高声压贝斯变体, 8 round-robin)
    bool loadSlapSamples(const std::string& wavDir);

    /// 触发播放一个采样 (type: 0=bass, 1=tone, 2=tip)
    /// velocity: 0-127
    void trigger(int type, float velocity);

    /// 触发 slap 采样 (力度>100时的贝斯变体)
    void triggerSlap(float velocity);

    /// 音频回调: 将活跃采样混入输出 buffer (由 AudioEngine 调用)
    /// outBuf: interleaved stereo float buffer
    /// numFrames: 帧数
    void mixAudio(float* outBuf, int32_t numFrames);

    /// 仅过混响 (供 BassSynth 使用, 独立 reverb 实例)
    void processBassReverb(float* buf, int32_t numFrames);

    /// 设置主音量增益 (0-4.0, 默认 3.2 = 80%)
    void setMasterGain(float g) { masterGain.store(g); }
    float getMasterGain() const { return masterGain.load(); }

    /// 设置混响: roomSize 0-1, level 0-1 (与 AudioEngine 参数一致)
    void setReverb(float roomSize, float level);

private:
    struct ActiveVoice {
        const float* data;    // PCM 数据指针
        size_t length;        // 采样数
        size_t position;      // 当前播放位置
        float gain;           // 音量 (0-1)
    };

    std::vector<WavSample> samples;
    std::vector<WavSample> slapSamples;
    std::vector<ActiveVoice> voices;
    std::mutex voiceMutex;
    std::atomic<float> masterGain{0.0f}; // 0-4.0, 默认 0%

    SimpleReverb reverb;
    SimpleReverb bassReverb;
    bool bassReverbInited = false;
    std::vector<float> tempBuf; // 混响临时 buffer
    bool reverbInited = false;

    // 低频 Bell EQ: 60Hz, Q≈0.375, +10dB
    // Biquad 状态 (stereo: L/R 各一组 x1/x2/y1/y2)
    float eq_x1L = 0, eq_x2L = 0, eq_y1L = 0, eq_y2L = 0;
    float eq_x1R = 0, eq_x2R = 0, eq_y1R = 0, eq_y2R = 0;
    float bassEq_x1L = 0, bassEq_x2L = 0, bassEq_y1L = 0, bassEq_y2L = 0;
    float bassEq_x1R = 0, bassEq_x2R = 0, bassEq_y1R = 0, bassEq_y2R = 0;
    float bassHiEq_x1L = 0, bassHiEq_x2L = 0, bassHiEq_y1L = 0, bassHiEq_y2L = 0;
    float bassHiEq_x1R = 0, bassHiEq_x2R = 0, bassHiEq_y1R = 0, bassHiEq_y2R = 0;
    void applyLowBellEQ(float* buf, int32_t numFrames);
    void applyBassEQ(float* buf, int32_t numFrames);
    void applyBassHiCutEQ(float* buf, int32_t numFrames);

    WavSample loadWav(const std::string& path);
};
