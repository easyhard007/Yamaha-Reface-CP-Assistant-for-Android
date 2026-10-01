#include "BeatTracker.h"
#include "CajonAssistant.h"
#include <algorithm>
#include <chrono>
#include <thread>
#include <android/log.h>

extern std::atomic<double> g_pendingBpmUpdate;
extern std::atomic<double> g_pendingRhythmGainUpdate;
extern std::atomic<double> g_pendingMinEnergyUpdate;
extern float g_minCajonEnergy;
class RhythmAudioEngine;
extern RhythmAudioEngine* g_rhythmEngine;
class CajonAssistant;
extern CajonAssistant cajon;

BeatTracker::BeatTracker() = default;

BeatTracker::~BeatTracker() { stop(); }

void BeatTracker::start(int bpb, double initBpm) {
    stop();
    beatsPerBar = bpb;
    bpm = initBpm;
    beatIntervalMs.store(60000.0 / bpm.load());
    currentBeat.store(0);
    needSync.store(false);
    running.store(true);
    beatThread = std::thread(&BeatTracker::beatLoop, this);
}

void BeatTracker::stop() {
    running.store(false);
    cv.notify_all();
    if (beatThread.joinable()) beatThread.join();
}

void BeatTracker::setTempo(double newBpm) {
    if (newBpm < 30 || newBpm > 300) return;
    bpm.store(newBpm);
    beatIntervalMs.store(60000.0 / newBpm);
}

void BeatTracker::sync() {
    needSync.store(true);
    cv.notify_one(); // 唤醒 beatLoop 立即跳到第 1 拍
}

double BeatTracker::tapTempo(bool autoStartCajon) {
    using namespace std::chrono;
    // 保留亚毫秒精度，避免四次 tap 的整数截断累积到启播相位。
    double nowMs = duration<double, std::milli>(
        steady_clock::now().time_since_epoch()).count();

    // 两种模式的 tap 序列不混用，避免切换模式后一击就误触发。
    if (mTapTempoCajonMode != autoStartCajon) {
        mTapTempoStamps.clear();
        mTapTempoCajonMode = autoStartCajon;
    }

    // 最新时间戳超过 2 秒 → 清空重新计数
    if (!mTapTempoStamps.empty() && (nowMs - mTapTempoStamps.back()) > 2000.0) {
        mTapTempoStamps.clear();
    }
    mTapTempoStamps.push_back(nowMs);

    if (mTapTempoStamps.size() >= 4) {
        // 计算相邻间隔均值的 4 倍 = measure_ms
        double sumInterval = 0.0;
        for (size_t i = 1; i < mTapTempoStamps.size(); i++) {
            sumInterval += mTapTempoStamps[i] - mTapTempoStamps[i - 1];
        }
        double avgInterval = sumInterval / (mTapTempoStamps.size() - 1);
        double measureMs = avgInterval * 4.0;
        double newBpm = 240000.0 / measureMs;
        int roundedBpm = (int)(newBpm + 0.5);
        if (roundedBpm < 30) roundedBpm = 30;
        if (roundedBpm > 300) roundedBpm = 300;

        setTempo(roundedBpm);
        __android_log_print(ANDROID_LOG_INFO, "BeatTracker",
            "TapTempo: %zu taps, avg=%.0fms, bpm=%d", mTapTempoStamps.size(), avgInterval, roundedBpm);

        // 推送 BPM 到 UI
        g_pendingBpmUpdate.store((double)roundedBpm);

        const double leadMs = autoStartCajon ? 50.0 : 100.0;
        const double triggerDelayMs = std::max(0.0, measureMs * 0.25 - leadMs);

        // 箱鼓模式保留原行为；鼓循环模式由 Kotlin 用返回的同一延时启播。
        if (autoStartCajon) {
            const int64_t delayMs = (int64_t)triggerDelayMs;
            BeatTracker* pSelf = this;
            std::thread([delayMs, pSelf]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));

                // 先开音量+能量, 确保 sync 第一拍 Cajon 有声
                if (g_rhythmEngine) g_rhythmEngine->setMasterGain(2.0f);
                g_pendingRhythmGainUpdate.store(2.0);
                g_minCajonEnergy = 0.4f;
                g_pendingMinEnergyUpdate.store(0.4);
                cajon.setEnergy(0.4f);

                // BPM 更新 UI
                g_pendingBpmUpdate.store(pSelf->getCurrentBpm());

                // 最后 sync, 此时音量+能量已就位
                pSelf->sync();
            }).detach();
        }

        // 每四击是一组完整测量，不把第五击累加到旧序列。
        mTapTempoStamps.clear();
        return triggerDelayMs;
    }
    return -1.0;
}

void BeatTracker::beatLoop() {
    using namespace std::chrono;
    auto nextBeatTime = steady_clock::now();
    int beat = 0;

    while (running.load()) {
        // SYNC 请求: 立即跳到第 1 拍
        if (needSync.exchange(false)) {
            auto syncAt = steady_clock::now();
            beat = 0;
            stepCounter = 0; // 重置步进计数器, 否则 groove 相位错位
            currentBeat.store(0);
            if (beatZeroSig) beatZeroSig->store(true);
            nextBeatTime = syncAt; // 立即开始 beat 0, 不等
            __android_log_print(ANDROID_LOG_INFO, "BeatTracker", "SYNC: beat 0 starts now, beatIntervalMs=%.0f", beatIntervalMs.load());
            continue;
        }

        // 等待到下一拍, 可被 setTempo/sync 中断
        {
            std::unique_lock<std::mutex> lock(mtx);
            auto status = cv.wait_until(lock, nextBeatTime);
            if (!running.load()) return;
            // 被 notify 唤醒 (tempo 变了) → 重算 interval, 但保持当前时间基准
            if (status == std::cv_status::no_timeout) {
                // tempo 或 sync 通知, 用当前时间作为新基准
                nextBeatTime = steady_clock::now();
                if (!needSync.load()) {
                    // 纯 tempo 变化: 跳到下一拍
                }
                // 重新 wait
                continue;
            }
        }

        // 8 个 32分音符子步, 按间隔依次发出 (带 Humanize 偏移)
        if (stepCb) {
            const double intervalMs = beatIntervalMs.load();
            double subMs = intervalMs / 8.0;
            for (int s = 0; s < 8; s++) {
                int step = stepCounter % 32;
                float humanizeMs = CajonAssistant::humanizeOffset(step) * 1000.0f;
                // beat 边界音符 (s==0) 只许滞后, 不许提前
                if (s == 0 && humanizeMs < 0) humanizeMs = 0;
                double delayMs = s * subMs + humanizeMs;
                if (delayMs < 0) delayMs = 0;
                auto targetUs = nextBeatTime + std::chrono::microseconds((int64_t)(delayMs * 1000));
                {
                    std::unique_lock<std::mutex> lock(mtx);
                    cv.wait_until(lock, targetUs);
                }
                if (!running.load()) { currentBeat.store(beat); return; }
                if (needSync.load()) { __android_log_print(ANDROID_LOG_INFO, "BeatTracker", "SYNC break at sub-beat s=%d step=%d", s, step); break; }
                stepCb(step, bpm.load(), stepCbUser);
                stepCounter++;
            }
        }
        beat = (beat + 1) % beatsPerBar;
        currentBeat.store(beat);
        if (beat == 0 && beatZeroSig) beatZeroSig->store(true);

        // 计算下一拍的时间
        nextBeatTime += milliseconds((int64_t)beatIntervalMs.load());
    }
}
