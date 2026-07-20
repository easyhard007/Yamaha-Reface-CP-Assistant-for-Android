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

    /// 节拍回调: 当前步进 (0-31), tempo BPM. 返回 bass 力度 (0=未触发)
    int onStep(int step, double bpm);

    /// 获取当前步进
    int getCurrentStep() const { return currentStep.load(); }

    // 最近一次 Bass 触发
    int getLastBassVel() const { return mLastBassVel; }
    double getLastBassTime() const { return mLastBassTime; }

    /// 获取 32 步的能量矩阵 (供 UI) — 返回 32 个 float权重值
    const float* getStepWeights() const { return stepWeights; }

    static float humanizeOffset(int step);
    static int getMetricWeight(int step);

private:
    RhythmAudioEngine* engine = nullptr;
    std::atomic<bool> enabled{false};
    std::atomic<float> energy{0.5f};
    std::atomic<int> currentStep{0};
    float stepWeights[32] = {};
    int mLastBassVel = 0;
    double mLastBassTime = 0;

    /// 规则引擎
    struct Hit { bool play; int velocity; };
    Hit grooveBass(int step, float energy, int weight);
    Hit grooveTone(int step, float energy, int weight);
    Hit grooveTip(int step, float energy, int weight);
};
