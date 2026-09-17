#pragma once

#include <vector>
#include <string>
#include <map>
#include <memory>
#include <mutex>
#include <atomic>
#include <cstdint>

class TempoStream;
class SignalsmithStream;

/// 鼓循环播放引擎: 双引擎渲染 + 无缝循环混音
///
/// 每个循环 (Loop):
///  - buf: 播放缓冲 (渲染中不断增长; 完整后可循环)
///  - stream: 高质量 sbsms 流式渲染 (预热/完整渲染线程驱动)
///  - sgsm:   低质量低延迟 Signalsmith 流式渲染 (点击未渲染 variation 时立即启动)
///  - pending*: 高质量重渲染缓冲 (播放中重新渲染, 完成后在循环边界切换)
///
/// 目标分离规则:
///  - sbsms 渲染在 "播放中 或 sgsm 活跃" 时进入 pending (避免与当前播放缓冲冲突)
///  - sgsm 启动时若 sbsms 渲染正在进行, 将其转入 pending, buf 让给 sgsm
class DrumLoopEngine {
public:
    struct Loop {
        int sampleRate = 48000;
        float ratio = 1.0f;
        int renderGen = -1;                               // sbsms 渲染批次
        int sgsmGen = -1;                                 // sgsm 渲染批次
        bool complete = false;                            // buf 完整可循环
        std::shared_ptr<std::vector<int16_t>> buf;        // 播放缓冲
        std::shared_ptr<TempoStream> stream;              // sbsms 渲染 (非 null = 渲染中)
        std::shared_ptr<SignalsmithStream> sgsm;          // sgsm 渲染 (非 null = 渲染中)
        std::shared_ptr<std::vector<int16_t>> pendingBuf;       // 高质量重渲染缓冲
        std::shared_ptr<TempoStream> pendingStream;             // 高质量重渲染流
        bool pendingComplete = false;
    };

    DrumLoopEngine() = default;
    ~DrumLoopEngine() = default;

    // ===== 高质量 (sbsms) 渲染 =====
    bool renderStart(const std::string& name, int sampleRate, float ratio, int gen);
    int64_t renderFeed(const std::string& name, const int16_t* pcm, int32_t totalSamples, int gen);
    bool renderFinish(const std::string& name, int gen);

    // ===== 低质量低延迟 (Signalsmith) 渲染 =====
    bool sgsmStart(const std::string& name, int sampleRate, float ratio, int gen);
    int64_t sgsmFeed(const std::string& name, const int16_t* pcm, int32_t totalSamples, int gen);
    bool sgsmFinish(const std::string& name, int gen);

    /// 取消所有进行中的渲染 (保留已渲染缓冲用于播放)
    void renderCancelAll();

    /// 清空所有循环并停止播放
    void clear();

    void play(const std::string& name);
    void stop();

    void setVolume(float v) { mVolume.store(v); }
    float getVolume() const { return mVolume.load(); }

    void setRate(float rate) { mRate.store(rate); }
    float getRate() const { return mRate.load(); }

    void mixAudio(float* outBuf, int32_t numFrames, int32_t deviceSampleRate);

private:
    std::mutex mLock;
    std::map<std::string, std::shared_ptr<Loop>> mLoops;
    std::shared_ptr<Loop> mActiveLoop;
    double mActivePos = 0.0;
    std::atomic<float> mVolume{0.9f};
    std::atomic<float> mRate{1.0f};
};
