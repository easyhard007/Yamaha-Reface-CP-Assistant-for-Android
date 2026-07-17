#pragma once
#include <vector>
#include <cstdint>
#include <tuple>

// 定义输入的 MIDI 三元组 (更名为 TempoMidiEvent 以避免与 StyleParser 的 MidiEvent 冲突)
struct TempoMidiEvent {
    uint8_t note;
    uint8_t velocity;
    double timestampMs;
};

class TempoTracker {
private:
    int numerator;   // 拍号分子 (如 4)
    int denominator; // 拍号分母 (如 4)

    double currentBpm;       // 当前系统预测的 BPM
    double expectedBeatTime; // 算法预测的下一个“正拍”应该发生的时间(毫秒)
    double beatIntervalMs;   // 当前一拍的理论毫秒数
    int currentBeatIndex;    // 当前是这一小节的第几拍 (0 到 numerator-1)

    // PLL 调节参数
    const double MIN_BPM = 40.0;
    const double MAX_BPM = 240.0;
    const double CLUSTER_WINDOW_MS = 30.0; // 把 30ms 内落下的音符视为同一个和弦/击键
    const double MAX_TOLERANCE_MS = 200.0; // 超过预测时间 200ms 的音符视为切分音，忽略测速

    void updatePLL(double actualTimeMs, double clusterVelocity);
    double getBeatWeight(int beatIndex) const;

public:
    TempoTracker();

    // 1. 初始化引擎，设定拍号和初始基准速度
    void init(int num, int den, double initialBpm, double startTimeMs);

    // 2. 核心预测函数：输入一批新产生的 MIDI 事件，返回修正后的预测 BPM
    double processEvents(const std::vector<TempoMidiEvent>& events);

    // 对齐节拍器: 将当前拍重置为第 1 拍 (beat 0)
    void syncBeat(double nowMs);

    // 获取当前状态
    double getCurrentBpm() const { return currentBpm; }
    double getExpectedNextBeatTime() const { return expectedBeatTime; }
    int getCurrentBeatIndex() const; // 基于墙钟和 BPM 实时计算当前拍位
};