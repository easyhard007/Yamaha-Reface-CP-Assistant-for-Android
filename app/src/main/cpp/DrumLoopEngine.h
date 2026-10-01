#pragma once

#include <vector>
#include <string>
#include <map>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <cstdint>
#include <cstddef>

class TempoStream;
class SignalsmithStream;

/// 鼓循环播放引擎: 双引擎渲染 + 无缝循环混音
///
/// 每个循环 (Loop) 把三类对象分开保存：
///  - buf / sgsmBuilds: Signalsmith 主缓冲及仍在增长的各 generation；
///  - pending*: 当前 SBSMS 构建缓冲；
///  - hq*: 已发布、可由播放槽安全引用的四级高质量快照。
/// 新一阶段/新速度的构建不会覆盖正在播放的旧 shared_ptr 快照。
class DrumLoopEngine {
public:
    using TempoCutCallback = void (*)(double bpm);
    using OneShotSwitchCallback = void (*)();
    enum BassMidiAction { BassAllSoundsOff = 0, BassNoteOn = 1, BassNoteOff = 2 };
    using BassMidiCallback = void (*)(int action, int pitch, int velocity);

    /// 渲染线程专用的固定容量缓冲区。
    ///
    /// samples 在构造后永不移动/扩容；渲染线程只写 publishedFrames 之后的区域，
    /// 完成一段写入后以 release 语义发布新水位。音频线程以 acquire 语义读取水位，
    /// 因而无需与渲染线程共同持有 mLock。
    struct PcmBuffer {
        explicit PcmBuffer(size_t requestedCapacityFrames);

        std::unique_ptr<int16_t[]> samples;
        size_t capacityFrames = 0;
        std::atomic<size_t> publishedFrames{0};
        std::atomic<bool> overflowLogged{false};

        bool valid() const { return samples && capacityFrames > 0; }
    };

    struct SgsmBuild {
        std::shared_ptr<SignalsmithStream> stream;
        std::shared_ptr<PcmBuffer> buf;
        bool drainStarted = false;
        bool complete = false;
    };

    struct BassMidiEvent {
        double phase = 0.0; // 0..1 on the variation's logical timeline
        int velocity = 0;   // 原始 MIDI 力度；0=note-off，C5 输出时再缩放到 60%
        bool downbeat = false; // true only for a note-on at the first beat of a measure
        bool strongBeat = false; // note-on within a 32nd note of beat 1 or beat 3
        int noteKind = 0;    // 0=C4, 1=isolated C5, 2=connected C5
        int phraseGroup = -1; // connected-C5 component id; otherwise -1
    };

    struct Loop {
        std::string name;
        int sampleRate = 48000;
        float ratio = 1.0f;
        int renderGen = -1;                               // sbsms 渲染批次
        int sgsmGen = -1;                                 // sgsm 渲染批次
        int bufGen = -1;                                  // buf 所属渲染批次（区分旧速度播放快照）
        bool complete = false;                            // buf 完整可循环
        double bufOriginFrame = 0.0;                      // buf 第 0 帧对应 variation 逻辑时间
        double bufLogicalFrames = 0.0;                    // 当前速度下 variation 的逻辑总帧数
        bool sbsmDrainStarted = false;                    // sbsms 排空阶段已开始 (finishInput 已调用)
        bool sgsmDrainStarted = false;                    // sgsm 排空阶段已开始 (finishInput 已调用)
        std::shared_ptr<PcmBuffer> buf;                   // Signalsmith 固定容量播放缓冲
        std::shared_ptr<TempoStream> stream;              // sbsms 渲染 (非 null = 渲染中)
        std::shared_ptr<SignalsmithStream> sgsm;          // sgsm 渲染 (非 null = 渲染中)
        std::map<int, std::shared_ptr<SgsmBuild>> sgsmBuilds; // 允许当前代与下一目标代并行流式生成
        std::shared_ptr<PcmBuffer> pendingBuf;                  // 高质量固定容量构建缓冲
        std::shared_ptr<TempoStream> pendingStream;             // 高质量重渲染流
        bool pendingComplete = false;
        bool pendingPlayable = false;                          // 高质量 pending 已达到安全起播水位
        double pendingOriginFrame = 0.0;
        double pendingLogicalFrames = 0.0;
        bool promoteAtWrap = false;                            // 预热完成: 循环边界切换为高质量缓冲

        // 已发布的 SBSMS 高质量快照。它与 pending 构建缓冲、Signalsmith 主缓冲彼此独立：
        // 新一阶段的 SBSMS 重建不会覆盖仍可播放的上一阶段快照。
        std::shared_ptr<PcmBuffer> hqBuf;
        int hqGen = -1;
        int hqState = 0;                                      // 0=无, 1=头0.5s, 2=头+末小节, 3=完整
        bool hqComplete = false;
        double hqOriginFrame = 0.0;                            // hqBuf 第0帧对应的逻辑时间
        double hqLogicalFrames = 0.0;
        std::vector<BassMidiEvent> bassMidi;                    // 与 variation 同相位的贝斯乐句
    };

    DrumLoopEngine();
    ~DrumLoopEngine();

    // ===== 高质量 (sbsms) 渲染 =====
    bool renderStart(const std::string& name, int sampleRate, float ratio, int gen,
                     double logicalOriginSec, double logicalDurationSec);
    int64_t renderFeed(const std::string& name, const int16_t* pcm, int32_t totalSamples, int gen);
    /// 排空一步 (协作式, 单次渲染至多 8192 帧); 返回本次渲染帧数, 0 = 排空完成, -1 = 批次过期
    int64_t renderFinishStep(const std::string& name, int gen);

    /// 把当前 SBSMS 构建缓冲发布为指定高质量状态；构建可继续向同一缓冲追加。
    bool renderCommit(const std::string& name, int gen, int state);

    /// 兼容旧 JNI：把当前 pending 缓冲发布为状态1快照；不再安排循环边界自动升级。
    bool promotePending(const std::string& name);

    // ===== 低质量低延迟 (Signalsmith) 渲染 =====
    bool sgsmStart(const std::string& name, int sampleRate, float ratio, int gen,
                   double logicalOriginSec, double logicalDurationSec);
    int64_t sgsmFeed(const std::string& name, const int16_t* pcm, int32_t totalSamples, int gen);
    /// 排空一步 (协作式); 返回本次渲染帧数, 0 = 排空完成, -1 = 批次过期
    int64_t sgsmFinishStep(const std::string& name, int gen);

    /// 使全部 HQ 构建/快照失效；播放槽与仍被引用的 Signalsmith generation 可继续增长。
    void renderCancelAll();

    /// 清空所有循环并停止播放
    void clear();

    void play(const std::string& name);
    /// 从 variation 逻辑时间轴的任意位置起播；高质量状态不足时回退到 Signalsmith 主缓冲。
    void playAt(const std::string& name, double logicalStartSec);
    void stop();
    bool isPlaying() const;

    /// 当前播放槽在 variation 逻辑时间轴上的位置（秒）；name 不匹配或未播放返回 -1。
    double getPlaybackPositionSec(const std::string& name) const;

    /// 在音频回调中按播放时间倒计时，并在 cut point 原子切换到同 variation 的新速度 buf。
    bool armTempoCut(const std::string& name, int gen, double delaySec,
                     const std::vector<double>& targetOffsetsSec,
                     const std::vector<double>& nextDelaysSec,
                     double targetBpm);
    void cancelTempoCut();
    double getAndClearTempoCutEvent();
    void setTempoCutCallback(TempoCutCallback cb) { mTempoCutCallback = cb; }

    /// 在音频回调的指定时刻，把当前 variation 以 30ms 等功率交叉淡化跳到
    /// state2/3 高质量缓冲中的最后一小节位置。
    bool armAutoFillJump(const std::string& name, int gen, double delaySec,
                         double destinationLogicalSec);
    void cancelAutoFillJump();

    /// 以等功率交叉淡化进入一次性 variation；delaySec>0 时继续播放当前循环，
    /// 由音频回调精确倒计时后才开始。播放到逻辑结尾后无缝切到 nextName 的
    /// 0 秒，或在 stopAfter=true 时停止。
    bool startOneShot(const std::string& name, double logicalStartSec, int fadeMs,
                      const std::string& nextName, bool stopAfter, double delaySec,
                      double endFadeSec);
    /// INTRO/BREAK 播放期间允许前端更改播完后的目标 variation。
    bool updateOneShotNext(const std::string& nextName);
    void cancelOneShot();
    std::string getAndClearOneShotStartedEvent();
    std::string getAndClearOneShotSwitchEvent();
    void setOneShotSwitchCallback(OneShotSwitchCallback cb) { mOneShotSwitchCallback = cb; }

    /// 注册 variation 同名 MIDI 中的音符事件。phase 按原始 variation 总时长归一化；
    /// velocity=0 表示 note-off，其余值保持 MIDI 原力度。
    bool setBassMidiEvents(const std::string& name, const std::vector<double>& phases,
                           const std::vector<int>& velocities,
                           const std::vector<int>& downbeats,
                           const std::vector<int>& strongBeats,
                           const std::vector<int>& noteKinds,
                           const std::vector<int>& phraseGroups);
    void setBassMidiCallback(BassMidiCallback cb) { mBassMidiCallback = cb; }
    /// 由原生 MIDI/和弦识别链即时推送全局和弦。有效和弦立即成为贝斯和弦；
    /// 无和弦只启动 5 秒可取消计时，避免演奏换把间隙让贝斯过早静音。
    /// fallbackPitch 是根音/转位低音；pitchClasses 包含 ChordDetector 给出的
    /// 全部和弦内音（含七、九音）。tempoBpm 用于补弹力度的音乐时值衰减。
    void setBassChordContext(int fallbackPitch, int rootPitch,
                             const std::vector<int>& pitchClasses, double tempoBpm);
    void setBassSelectionEnergy(float energy);
    void setBassLoopEnabled(bool enabled);

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
        std::shared_ptr<PcmBuffer> buf;             // 固定容量播放快照；可读范围由 publishedFrames 发布
        int bufGen = -1;                           // 快照所属批次，防止新速度缓冲提前接管旧速度播放
        int sampleRate = 48000;
        double logicalOriginFrame = 0.0;
        double logicalDurationFrames = 0.0;
        bool complete = false;                         // 此播放缓冲快照是否已经完整，不能借用 Loop 的新批次状态
        int hqState = 0;                               // 0=Signalsmith/普通缓冲, 1..3=高质量快照
        double pos = 0.0;
        float gain = 1.0f;
        bool fading = false;
        int64_t fadeTotalUs = 0;
        int64_t fadeRemainingUs = 0;
        int64_t fadeInRemainingUs = 0;   // 新槽淡入 (10ms)
        // Auto Fill 专用 constant-power 包络：-1=cos 淡出，+1=sin 淡入。
        int constantPowerRole = 0;
        int64_t constantPowerTotalFrames = 0;
        int64_t constantPowerProgressFrames = 0;
    };
    Slot mSlots[2];
    int mCurSlot = 0;
    static constexpr int64_t kSwitchFadeUs = 30000;   // 切换淡出 30ms
    static constexpr int64_t kSwitchFadeInUs = 10000; // 新槽淡入 10ms

    struct PlaybackSource {
        std::shared_ptr<Loop> loop;
        std::shared_ptr<PcmBuffer> buf;
        int bufGen = -1;
        int sampleRate = 48000;
        double logicalOriginFrame = 0.0;
        double logicalDurationFrames = 0.0;
        double logicalStartFrame = 0.0;
        double pos = 0.0;
        bool complete = false;
        int hqState = 0;
    };

    // 任意 cut point 变速计划。remainingSec 由 Oboe 实际输出帧递减，不依赖 UI 轮询。
    bool mTempoCutArmed = false;
    std::string mTempoCutName;
    int mTempoCutGen = -1;
    double mTempoCutRemainingSec = 0.0;
    double mTempoCutTargetBpm = -1.0;
    struct TempoCutCandidate {
        double targetOffsetSec = 0.0;  // 相对目标旋转缓冲区开头的物理播放位置
        double nextDelaySec = 0.0;     // 本切分点失败后，到下一个切分点的旧速度时间
    };
    std::vector<TempoCutCandidate> mTempoCutCandidates;
    size_t mTempoCutCandidateIndex = 0;
    std::atomic<double> mTempoCutEvent{-1.0};
    TempoCutCallback mTempoCutCallback = nullptr;

    // Auto Fill 的近即时切点跳转；与变速 cut 互斥，倒计时同样由实际输出帧推进。
    bool mAutoFillJumpArmed = false;
    std::string mAutoFillJumpName;
    int mAutoFillJumpGen = -1;
    double mAutoFillJumpRemainingSec = 0.0;
    double mAutoFillJumpDestinationSec = 0.0;

    // Smart Fill / BREAK / INTRO / ENDING 共用的一次性播放状态。延迟启动按设备输出帧，
    // 播放结束按源音频帧倒计时，两端都不依赖 Kotlin 定时器。
    bool mOneShotStartArmed = false;
    double mOneShotStartRemainingSec = 0.0;
    std::string mOneShotPendingName;
    double mOneShotPendingLogicalStartSec = 0.0;
    int mOneShotPendingFadeMs = 30;
    std::string mOneShotPendingNextName;
    bool mOneShotPendingStopAfter = false;
    double mOneShotPendingEndFadeSec = 0.0;
    PlaybackSource mOneShotPendingSource;
    PlaybackSource mOneShotPendingNext;
    bool mOneShotActive = false;
    bool mOneShotStopAfter = false;
    std::string mOneShotName;
    double mOneShotRemainingSourceFrames = 0.0;
    double mOneShotEndFadeSourceFrames = 0.0;
    PlaybackSource mOneShotNext;
    bool mOneShotNextReady = false;
    std::atomic<int> mOneShotStartedEvent{0};
    std::string mOneShotStartedEventName;
    std::atomic<int> mOneShotSwitchEvent{0};
    std::string mOneShotSwitchEventName;
    OneShotSwitchCallback mOneShotSwitchCallback = nullptr;
    std::atomic<int32_t> mLastDeviceSampleRate{48000};

    // 贝斯循环与当前播放槽共用同一逻辑时间轴。这里只保存 transport 状态；
    // 实际发声仍通过 AudioEngine 的 bass synth（target=2）。
    BassMidiCallback mBassMidiCallback = nullptr;
    std::atomic<int> mChordBassPitch{-1};
    std::vector<int> mBassChordPitches;
    std::vector<int> mBassChordTonePcs;
    int mBassChordRootPc = -1;
    int mBassChordThirdPc = -1;
    int mBassChordFifthPc = -1;
    double mBassTempoBpm = 75.0;
    bool mGlobalBassChordPresent = false;
    int64_t mBassNoChordDeadlineUs = -1;
    bool mBassChordTimerStopping = false;
    std::condition_variable mBassChordTimerCv;
    std::thread mBassChordTimerThread;
    std::atomic<float> mBassSelectionEnergy{0.0f};
    uint32_t mBassRandomState = 0x6d2b79f5u;
    std::atomic<bool> mBassLoopEnabled{false};
    const Loop* mBassMidiLoop = nullptr;
    int mBassMidiBufGen = -1;
    double mBassMidiDurationFrames = 0.0;
    double mBassMidiLastLogicalFrame = -1.0;
    size_t mBassMidiNextEvent = 0;
    bool mBassMidiNeedsSync = true;
    bool mBassMidiInputOn = false;
    int mBassMidiInputVelocity = 0;
    bool mBassMidiInputDownbeat = false;
    bool mBassMidiInputStrongBeat = false;
    int mBassMidiInputKind = 0;
    int mBassMidiInputGroup = -1;
    int64_t mBassMidiGateStartUs = -1;
    double mBassMidiGateEndLogicalFrame = -1.0;
    int mBassMidiOutputPitch = -1;
    int mBassConnectedGroup = -1;
    int mBassConnectedPreviousPitch = -1;

    /// 混音一个播放槽 (须在 mLock 持有下调用)
    void mixSlot(Slot& slot, bool isCurrent, float* outBuf, int32_t numFrames,
                 int32_t deviceSampleRate, float baseVol);

    /// mLock 内调用；把当前槽切到已准备的新速度主缓冲，返回已应用 BPM，失败返回 -1。
    double fireTempoCutLocked();

    /// mLock 内、音频回调中调用；建立同 variation A/B 双槽等功率淡化。
    bool fireAutoFillJumpLocked(int32_t deviceSampleRate);

    /// mLock 内调用：把逻辑时间映射到当前可播放的 SBSMS/Signalsmith 快照。
    bool preparePlaybackSourceLocked(const std::string& name, double logicalStartSec,
                                     PlaybackSource& out);
    /// mLock 内、音频回调中调用；返回 1=切换到 next，2=停止，0=无动作。
    int fireOneShotLocked();
    /// mLock 内调用；把已准备的一次性源安装进双槽并启动其结尾倒计时。
    bool beginOneShotLocked(PlaybackSource source, int fadeMs, PlaybackSource next,
                            bool stopAfter, int32_t deviceSampleRate, double endFadeSec);
    /// mLock 内、音频回调中调用；执行延迟的一次性段落启动。
    bool fireOneShotStartLocked(int32_t deviceSampleRate);
    void clearOneShotLocked();

    void resetBassMidiLocked(bool emitAllSoundsOff);
    void syncBassMidiLocked(const Slot& slot, double logicalFrame);
    void processBassMidiLocked(const Slot& slot, double logicalFrame, double step);
    void emitBassMidiEventLocked(const Loop& loop, size_t eventIndex);
    int chooseBassPitchLocked(int noteKind, bool downbeat, int phraseGroup);
    int chooseHighC5PitchLocked(int phraseGroup);
    float nextBassRandomLocked();
    int bassCatchUpVelocityLocked() const;
    int bassOutputVelocityLocked(int noteKind, int sourceVelocity) const;
    double findBassGateEndFrameLocked(const Loop& loop, size_t noteOnIndex) const;
    double bassGateRemainingBeatsLocked() const;
    bool bassPitchFitsCurrentTriadLocked(int pitch) const;
    void clearRetainedBassChordLocked();
    void bassChordTimerLoop();

    /// 该循环是否在任一播放槽中 (须在 mLock 持有下调用)
    bool isLoopActive(const std::shared_ptr<Loop>& loop) const {
        return mSlots[0].loop == loop || mSlots[1].loop == loop;
    }
};
