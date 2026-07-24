#pragma once
#include "RhythmAudioEngine.h"
#include <atomic>
#include <vector>
#include <mutex>

class CajonAssistant {
public:
    CajonAssistant();
    ~CajonAssistant();

    void init(RhythmAudioEngine* engine);

    void setEnabled(bool e) { enabled.store(e); }
    void setEnergy(float e) { energy.store(e); }
    float getEnergy() const { return energy.load(); }

    int getCurrentStep() const { return currentStep.load(); }
    int getLastBassVel() const { return mLastBassVel; }
    double getLastBassTime() const { return mLastBassTime; }
    int getLastToneVel() const { return mLastToneVel; }
    const float* getStepWeights() const { return stepWeights; }

    static float humanizeOffset(int step);
    static int getMetricWeight(int step);

    // ==== 每子步主入口: 触发采样 + 更新能量 ====
    // 返回 bass 力度 (0=未触发), energy 由外部计算后 setEnergy() 再传入
    int processStep(int step, double bpm);

    // ==== 能量衰减引擎 (每子步调用, 自动清理过期时间戳) ====
    float updateEnergy(double nowMs, double currentBpm);
    void feedNoteOn(double nowMs);

private:
    RhythmAudioEngine* engine = nullptr;
    std::atomic<bool> enabled{false};
    std::atomic<float> energy{0.5f};
    std::atomic<int> currentStep{0};
    float stepWeights[32] = {};
    int mLastBassVel = 0;
    double mLastBassTime = 0;
    int mLastToneVel = 0;

    // 能量计算
    std::vector<double> mNoteTimestamps;
    std::mutex mEnergyMutex;

    struct Hit { bool play; int velocity; };
    Hit grooveBass(int step, float energy, int weight);
    Hit grooveTone(int step, float energy, int weight);
    Hit grooveTip(int step, float energy, int weight);
};
