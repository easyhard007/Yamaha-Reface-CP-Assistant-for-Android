#pragma once
#include <vector>
#include <string>
#include <atomic>
#include <mutex>

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

    /// 触发播放一个采样 (type: 0=bass, 1=tone, 2=tip)
    /// velocity: 0-127
    void trigger(int type, float velocity);

    /// 音频回调: 将活跃采样混入输出 buffer (由 AudioEngine 调用)
    /// outBuf: interleaved stereo float buffer
    /// numFrames: 帧数
    void mixAudio(float* outBuf, int32_t numFrames);

private:
    struct ActiveVoice {
        const float* data;    // PCM 数据指针
        size_t length;        // 采样数
        size_t position;      // 当前播放位置
        float gain;           // 音量 (0-1)
    };

    std::vector<WavSample> samples;
    std::vector<ActiveVoice> voices;
    std::mutex voiceMutex;

    WavSample loadWav(const std::string& path);
};
