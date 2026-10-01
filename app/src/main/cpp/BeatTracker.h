#pragma once
#include <thread>
#include <atomic>
#include <cstdint>
#include <condition_variable>
#include <mutex>

class BeatTracker {
public:
    BeatTracker();
    ~BeatTracker();

    void setBeatZeroSignal(std::atomic<bool>* sig) { beatZeroSig = sig; }
    void setStepCallback(void (*cb)(int step, double bpm, void*), void* user) { stepCb = cb; stepCbUser = user; }

    /// 初始化拍号和 BPM, 启动节拍线程
    void start(int beatsPerBar, double bpm);

    /// 停止节拍线程
    void stop();

    /// 动态更新速度 (中断当前 sleep, 立即用新 BPM 重算)
    void setTempo(double newBpm);

    /// 重置到第 1 拍 (SYNC / START 时调用)
    void sync();

    /// Tap Tempo: 记录时间戳, ≥4次后计算并更新 BPM。
    /// 返回第四击后到下一小节的预先触发延时；未完成时返回 -1。
    /// 箱鼓使用 50ms、鼓循环使用 100ms 提前量抵消起播延迟。
    /// autoStartCajon=false 仅计算 BPM，不改变箱鼓音量/锁定状态。
    double tapTempo(bool autoStartCajon = true);

    /// 获取当前拍号 (0-based, 供 JS 轮询)
    int  getCurrentBeat() const { return currentBeat.load(); }
    double getCurrentBpm() const { return bpm.load(); }

    /// 是否正在运行
    bool isRunning() const { return running.load(); }

private:
    std::thread beatThread;
    std::atomic<bool> running{false};
    std::atomic<bool> needSync{false};
    std::atomic<int>  currentBeat{0};
    std::atomic<bool>* beatZeroSig = nullptr;
    void (*stepCb)(int step, double bpm, void*) = nullptr;
    void* stepCbUser = nullptr;
    int stepCounter = 0;

    int    beatsPerBar = 4;
    std::atomic<double> bpm{75.0};
    std::atomic<double> beatIntervalMs{800.0}; // 60000 / 75 = 800
    std::vector<double> mTapTempoStamps;
    bool mTapTempoCajonMode = true;

    std::mutex mtx;
    std::condition_variable cv;

    void beatLoop();
};
