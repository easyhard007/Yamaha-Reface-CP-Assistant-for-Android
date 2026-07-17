#pragma once
#include "RhythmAudioEngine.h"
#include <atomic>

class CajonAssistant {
public:
    CajonAssistant();
    ~CajonAssistant();

    void init(RhythmAudioEngine* engine);
    void setEnabled(bool e) { enabled.store(e); }

    /// 更新能量 (0.0-1.0)
    void setEnergy(float e) { energy.store(e); }
    float getEnergy() const { return energy.load(); }

    /// 节拍回调: 当前步进 (0-31), tempo BPM
    void onStep(int step, double bpm);

    /// 获取当前步进
    int getCurrentStep() const { return currentStep.load(); }

    /// 获取 32 步的能量矩阵 (供 UI) — 返回 32 个 float权重值
    const float* getStepWeights() const { return stepWeights; }

    static float humanizeOffset(int step);

private:
    RhythmAudioEngine* engine = nullptr;
    std::atomic<bool> enabled{false};
    std::atomic<float> energy{0.5f};
    std::atomic<int> currentStep{0};
    float stepWeights[32] = {};

    static int getMetricWeight(int step);

    /// 规则引擎
    struct Hit { bool play; int velocity; };
    Hit grooveBass(int step, float energy, int weight);
    Hit grooveTone(int step, float energy, int weight);
    Hit grooveTip(int step, float energy, int weight);
};
