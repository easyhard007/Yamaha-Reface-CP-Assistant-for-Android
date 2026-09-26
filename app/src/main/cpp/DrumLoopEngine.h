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
        std::string name;
        int sampleRate = 48000;
        float ratio = 1.0f;
        int renderGen = -1;                               // sbsms 渲染批次
        int sgsmGen = -1;                                 // sgsm 渲染批次
        int bufGen = -1;                                  // buf 所属渲染批次（区分旧速度播放快照）
        bool complete = false;                            // buf 完整可循环
        bool sbsmDrainStarted = false;                    // sbsms 排空阶段已开始 (finishInput 已调用)
        bool sgsmDrainStarted = false;                    // sgsm 排空阶段已开始 (finishInput 已调用)
        std::shared_ptr<std::vector<int16_t>> buf;        // 播放缓冲
        std::shared_ptr<TempoStream> stream;              // sbsms 渲染 (非 null = 渲染中)
        std::shared_ptr<SignalsmithStream> sgsm;          // sgsm 渲染 (非 null = 渲染中)
        std::shared_ptr<std::vector<int16_t>> pendingBuf;       // 高质量重渲染缓冲
        std::shared_ptr<TempoStream> pendingStream;             // 高质量重渲染流
        bool pendingComplete = false;
        bool pendingPlayable = false;                          // 高质量 pending 已达到安全起播水位
        bool promoteAtWrap = false;                            // 预热完成: 循环边界切换为高质量缓冲
    };

    DrumLoopEngine() = default;
    ~DrumLoopEngine() = default;

    // ===== 高质量 (sbsms) 渲染 =====
    bool renderStart(const std::string& name, int sampleRate, float ratio, int gen);
    int64_t renderFeed(const std::string& name, const int16_t* pcm, int32_t totalSamples, int gen);
    /// 排空一步 (协作式, 单次渲染至多 8192 帧); 返回本次渲染帧数, 0 = 排空完成, -1 = 批次过期
    int64_t renderFinishStep(const std::string& name, int gen);

    /// 预热完成: 标记在当前循环边界把播放缓冲切换为高质量 pending 缓冲
    /// (不打断当前播放; 正在播放低质量版本时使用)
    bool promotePending(const std::string& name);

    // ===== 低质量低延迟 (Signalsmith) 渲染 =====
    bool sgsmStart(const std::string& name, int sampleRate, float ratio, int gen);
    int64_t sgsmFeed(const std::string& name, const int16_t* pcm, int32_t totalSamples, int gen);
    /// 排空一步 (协作式); 返回本次渲染帧数, 0 = 排空完成, -1 = 批次过期
    int64_t sgsmFinishStep(const std::string& name, int gen);

    /// 取消所有进行中的渲染 (保留已渲染缓冲用于播放)
    void renderCancelAll();

    /// 清空所有循环并停止播放
    void clear();

    void play(const std::string& name);
    void stop();
    bool isPlaying() const;

    /// 淡出停止 (durationMs 内线性衰减到 0 并停止)
    void fadeOut(int durationMs);

    /// 记录切换触发时刻 (play 时自动调用), 混音器在首次出声时计算延迟
    int64_t getAndClearLatencyMs();   // -1 = 无

    /// 淡出完成事件 (Kotlin 轮询)
    int getAndClearStoppedEvent();

    void setVolume(float v) { mVolume.store(v); }
    float getVolume() const { return mVolume.load(); }

    void setRate(float rate) { mRate.store(rate); }
    float getRate() const { return mRate.load(); }

    void mixAudio(float* outBuf, int32_t numFrames, int32_t deviceSampleRate);

private:
    mutable std::mutex mLock;
    std::map<std::string, std::shared_ptr<Loop>> mLoops;
    std::atomic<float> mVolume{0.9f};
    std::atomic<float> mRate{1.0f};
    // 切换延迟测量
    std::atomic<int64_t> mTriggerUs{-1};
    std::atomic<int64_t> mLatencyMs{-1};
    // 全局淡出 (停止按钮)
    std::atomic<int> mFading{0};
    int64_t mFadeTotalUs = 0;
    int64_t mFadeRemainingUs = 0;
    std::atomic<int> mStoppedEvent{0};   // 淡出完成事件 (Kotlin 轮询)

    // 双播放槽 (ping-pong): 切换时新槽立即播放新循环, 旧槽 30ms 淡出后清空
    struct Slot {
        std::shared_ptr<Loop> loop;
        std::shared_ptr<std::vector<int16_t>> buf;  // 播放缓冲快照 (流式渲染持续增长, 快照不随 loop->buf 替换而变)
        int bufGen = -1;                           // 快照所属批次，防止新速度缓冲提前接管旧速度播放
        double pos = 0.0;
        float gain = 1.0f;
        bool fading = false;
        int64_t fadeTotalUs = 0;
        int64_t fadeRemainingUs = 0;
        int64_t fadeInRemainingUs = 0;   // 新槽淡入 (10ms)
    };
    Slot mSlots[2];
    int mCurSlot = 0;
    static constexpr int64_t kSwitchFadeUs = 30000;   // 切换淡出 30ms
    static constexpr int64_t kSwitchFadeInUs = 10000; // 新槽淡入 10ms

    /// 混音一个播放槽 (须在 mLock 持有下调用)
    void mixSlot(Slot& slot, bool isCurrent, float* outBuf, int32_t numFrames,
                 int32_t deviceSampleRate, float baseVol);

    /// 该循环是否在任一播放槽中 (须在 mLock 持有下调用)
    bool isLoopActive(const std::shared_ptr<Loop>& loop) const {
        return mSlots[0].loop == loop || mSlots[1].loop == loop;
    }
};
