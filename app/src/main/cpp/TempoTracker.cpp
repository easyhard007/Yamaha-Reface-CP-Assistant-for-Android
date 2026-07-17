#include "TempoTracker.h"
#include <algorithm>
#include <cmath>
#include <chrono>

TempoTracker::TempoTracker() : numerator(4), denominator(4), currentBpm(75.0),
                               expectedBeatTime(0), beatIntervalMs(800.0), currentBeatIndex(0) {}

void TempoTracker::init(int num, int den, double initialBpm, double startTimeMs) {
    this->numerator = (num > 0) ? num : 4;
    this->denominator = (den > 0) ? den : 4;
    this->currentBpm = initialBpm;

    // 计算一拍的毫秒数 (基于 4分音符为一拍的假设进行转换)
    double quarterNoteMs = 60000.0 / currentBpm;
    this->beatIntervalMs = quarterNoteMs * (4.0 / denominator);

    this->expectedBeatTime = startTimeMs + beatIntervalMs;
    this->currentBeatIndex = 0;
}

// 辅助函数：根据拍号给予不同的容错权重
// 例如 4/4 拍：第 1 拍(Index 0)最重，第 3 拍其次，弱拍较小
double TempoTracker::getBeatWeight(int beatIndex) const {
    if (beatIndex == 0) return 1.0; // 强拍

    if (numerator == 4 && beatIndex == 2) return 0.8; // 次强拍
    if (numerator == 6 && beatIndex == 3) return 0.8; // 6/8拍的次强拍

    return 0.4; // 弱拍
}

// 核心 PLL 修正算法
void TempoTracker::updatePLL(double actualTimeMs, double clusterVelocity) {
    // 1. 计算相位误差 (Error = 人类时间 - 预测时间)
    double phaseError = actualTimeMs - expectedBeatTime;

    // 2. 过滤掉偏离太远的音符 (认定为切分音或经过音，不影响主轴测速)
    if (std::abs(phaseError) > MAX_TOLERANCE_MS) {
        return;
    }

    // 3. 计算综合修正权重 (Alpha)
    // 力度越大，人类“拉扯”节奏的力量越强
    // 拍子位置越重要，算法越愿意信任它
    double velFactor = std::min(clusterVelocity / 127.0, 1.0); // 规一化 0.0 ~ 1.0 (和弦累加可能超过127，上限1.0)
    double beatWeight = getBeatWeight(currentBeatIndex);

    // Alpha 是 PLL 算法的阻尼系数，决定了跟手的平滑度 (通常在 0.05 ~ 0.3 之间)
    double alpha = 0.15 * velFactor * beatWeight;

    // 4. 更新下一次预测的绝对时间轴 (相位修正)
    expectedBeatTime += (phaseError * alpha);

    // 5. 更新 BPM 周期 (频率修正)
    // 如果 Error < 0 (抢拍，说明人弹得快，BPM应变大，周期间隔变短)
    // 利用微小的 Beta 系数慢慢改变实际频率
    double beta = 0.05 * velFactor;
    beatIntervalMs += (phaseError * beta);

    // 限制周期并反推新的 BPM
    currentBpm = (60000.0 * (4.0 / denominator)) / beatIntervalMs;

    // 限制极限速度，防止算法崩溃
    if (currentBpm < MIN_BPM) { currentBpm = MIN_BPM; beatIntervalMs = 60000.0 * (4.0 / denominator) / MIN_BPM; }
    if (currentBpm > MAX_BPM) { currentBpm = MAX_BPM; beatIntervalMs = 60000.0 * (4.0 / denominator) / MAX_BPM; }
}

int TempoTracker::getCurrentBeatIndex() const {
    using namespace std::chrono;
    double nowMs = duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
    double elapsed = nowMs - expectedBeatTime;
    int beatsPassed = (beatIntervalMs > 0) ? (int)(elapsed / beatIntervalMs) : 0;
    int idx = currentBeatIndex + beatsPassed;
    if (idx < 0) idx = 0;
    return idx % numerator;
}

void TempoTracker::syncBeat(double nowMs) {
    currentBeatIndex = 0;
    expectedBeatTime = nowMs + beatIntervalMs;
}

double TempoTracker::processEvents(const std::vector<TempoMidiEvent>& events) {
    if (events.empty()) return currentBpm;

    // 假设 events 已经按 timestamp 升序排序
    // (如果外部未排序，请在此处解开下面的排序代码)
    /*
    std::vector<MidiEvent> sortedEvents = events;
    std::sort(sortedEvents.begin(), sortedEvents.end(),
              [](const MidiEvent& a, const MidiEvent& b) { return a.timestampMs < b.timestampMs; });
    */

    // 1. 和弦聚类 (Onset Clustering)
    // 玩家按下和弦时，几个键的时间差极小。将 30ms 内的音符视为同一次打击
    double clusterStartTime = events[0].timestampMs;
    double clusterVelocitySum = 0;

    for (const auto& ev : events) {
        if (ev.velocity < 10) continue; // 过滤极轻的杂音/鬼音

        if (ev.timestampMs - clusterStartTime <= CLUSTER_WINDOW_MS) {
            clusterVelocitySum += ev.velocity;
        } else {
            // 一个和弦簇结束，喂给 PLL 算法进行速度推断
            updatePLL(clusterStartTime, clusterVelocitySum);

            // 开启下一个簇
            clusterStartTime = ev.timestampMs;
            clusterVelocitySum = ev.velocity;
        }

        // 推演内部时钟：如果当前音符的时间已经越过了下一个预测节拍，
        // 说明我们该“翻篇”到下一拍了。
        while (clusterStartTime > expectedBeatTime + (beatIntervalMs * 0.5)) {
            expectedBeatTime += beatIntervalMs;
            currentBeatIndex = (currentBeatIndex + 1) % numerator;
        }
    }

    // 处理最后一个收尾的簇
    if (clusterVelocitySum > 0) {
        updatePLL(clusterStartTime, clusterVelocitySum);
        while (clusterStartTime > expectedBeatTime + (beatIntervalMs * 0.5)) {
            expectedBeatTime += beatIntervalMs;
            currentBeatIndex = (currentBeatIndex + 1) % numerator;
        }
    }

    return currentBpm;
}