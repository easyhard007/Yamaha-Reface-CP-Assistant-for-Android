#include "BeatTracker.h"
#include "CajonAssistant.h"
#include <chrono>
#include <android/log.h>

BeatTracker::BeatTracker() = default;

BeatTracker::~BeatTracker() { stop(); }

void BeatTracker::start(int bpb, double initBpm) {
    stop();
    beatsPerBar = bpb;
    bpm = initBpm;
    beatIntervalMs = 60000.0 / bpm;
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
    bpm = newBpm;
    beatIntervalMs = 60000.0 / bpm;
    cv.notify_one(); // 唤醒 beatLoop 重算
}

void BeatTracker::sync() {
    needSync.store(true);
    cv.notify_one(); // 唤醒 beatLoop 立即跳到第 1 拍
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
            __android_log_print(ANDROID_LOG_INFO, "BeatTracker", "SYNC: beat 0 starts now, beatIntervalMs=%.0f", beatIntervalMs);
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
            double subMs = beatIntervalMs / 8.0;
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
                stepCb(step, bpm, stepCbUser);
                stepCounter++;
            }
        }
        beat = (beat + 1) % beatsPerBar;
        currentBeat.store(beat);
        if (beat == 0 && beatZeroSig) beatZeroSig->store(true);

        // 计算下一拍的时间
        nextBeatTime += milliseconds((int64_t)beatIntervalMs);
    }
}
