#include "CajonAssistant.h"
#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdlib>
#include <android/log.h>

CajonAssistant::CajonAssistant() {
    for (int i = 0; i < 32; i++) stepWeights[i] = (float)getMetricWeight(i) / 4.0f;
}
CajonAssistant::~CajonAssistant() = default;

void CajonAssistant::init(RhythmAudioEngine* eng) { engine = eng; }

int CajonAssistant::getMetricWeight(int step) {
    if (step % 8 == 0) return 4;
    if (step % 4 == 0) return 3;
    if (step % 2 == 0) return 2;
    return 1;
}

float CajonAssistant::humanizeOffset(int step) {
    // Box-Muller simplified
    float u = (rand() % 10000) / 10000.0f + 0.0001f;
    float v = (rand() % 10000) / 10000.0f + 0.0001f;
    float g = std::sqrt(-2.0f * std::log(u)) * std::cos(6.283185f * v);
    float maxOff = (step % 8 == 0) ? 0.005f : 0.010f;
    float sigma = maxOff / 3.0f;
    float off = g * sigma;
    if (off < -maxOff) off = -maxOff;
    if (off > maxOff) off = maxOff;
    return off;
}

CajonAssistant::Hit CajonAssistant::grooveBass(int step, float e, int w) {
    Hit h{}; h.play = false; h.velocity = 0;
    if (step == 0) { h.play = true; h.velocity = 25 + (int)(80*e); }
    else if (step == 16) { if ((rand()%10000)/10000.0f < e*0.8f) { h.play = true; h.velocity = 25 + (int)(80*e); } }
    else if (w >= 2 && step != 8 && step != 24) {
        bool firstHalf = step < 16;
        float dm = firstHalf ? 0.5f : 1.0f;
        float prob = (-0.2f + e*0.8f) * dm;
        if (step == 14 || step == 22) prob = -0.3f + e*0.9f;  // ~55% @e=0.94
        if ((rand()%10000)/10000.0f < prob) { h.play = true; h.velocity = 20 + w*10 + (int)(e*60); }
    } else if (w == 1 && e >= 0.8f) {
        bool firstHalf = step < 16;
        float dm = firstHalf ? 0.3f : 1.0f;
        if ((rand()%10000)/10000.0f < (e-0.7f)*0.25f*dm) { h.play = true; h.velocity = 90; }
    }
    if (h.play) h.velocity = std::min(127, h.velocity + rand()%10);
    return h;
}

CajonAssistant::Hit CajonAssistant::grooveTone(int step, float e, int w) {
    Hit h{}; h.play = false; h.velocity = 0;
    if (step == 0 || step == 16) return h;
    if (step == 8 || step == 24) { h.play = true; h.velocity = 35 + (int)(85*e); }
    else if (w == 2 || w == 3) {
        bool firstHalf = step < 16;
        float dm = firstHalf ? 0.6f : 1.0f;
        float prob = (-0.5f + e*1.0f) * dm;
        if (step == 18) prob = -0.2f + e*0.8f;      // e=0.25→0%, e=1.0→60%
        else if (step == 26) prob = -0.3f + e*1.0f;  // e=0.3→0%, e=1.0→70%
        if ((rand()%10000)/10000.0f < prob) {
            h.play = true;
            if (step == 18 || step == 26)
                h.velocity = 25 + (int)(e*80);  // e=1.0→105+rand(8)≈110
            else
                h.velocity = 20 + (int)(e*30);
        }
    } else if (w == 1) {
        bool firstHalf = step < 16;
        float dm = firstHalf ? 0.4f : 1.0f;
        float prob = 0;
        if (step == 7 || step == 23) prob = (-0.4f + e*1.2f)*dm;
        else if (e >= 0.7f) prob = (e-0.7f)*0.5f*dm;
        if ((rand()%10000)/10000.0f < prob) { h.play = true; h.velocity = 15 + (int)(e*25); }
    }
    if (h.play) h.velocity = std::min(127, h.velocity + rand()%8);
    return h;
}

CajonAssistant::Hit CajonAssistant::grooveTip(int step, float e, int w) {
    Hit h{}; h.play = false; h.velocity = 0;
    bool firstHalf = step < 16;
    float dm = firstHalf ? 0.6f : 1.0f;
    float prob = 0;
    if (w == 4) prob = (0.6f + e*0.4f) * (firstHalf ? 0.8f : 1.0f);
    else if (w == 3) prob = (0.1f + e*0.9f) * dm;
    else if (w == 2) prob = (-0.3f + e*1.3f) * dm;
    else if (w == 1 && e >= 0.7f) prob = (e-0.7f)*0.6f*dm;
    if ((rand()%10000)/10000.0f < prob) {
        h.play = true; h.velocity = 10 + w*5 + (int)(e*25) + rand()%5;
    }
    return h;
}

int CajonAssistant::processStep(int step, double /*bpm*/) {
    currentStep.store(step);
    if (!enabled.load() || !engine) return 0;
    float e = energy.load();
    int w = getMetricWeight(step);

    auto bass = grooveBass(step, e, w);
    auto tone = grooveTone(step, e, w);
    if (tone.play && (step != 8 && step != 24))
        __android_log_print(ANDROID_LOG_INFO, "Cajon", "TONE step=%d vel=%d e=%.2f w=%d", step, tone.velocity, e, w);
    if (e <= 0.0f) { bass.velocity = 0; tone.velocity = 0; }
    int hands = 0;
    int bassVel = 0;
    // Bass/Tone 互斥: tone 优先
    int toneVel = 0;
    if (tone.play && tone.velocity > 0) {
        // Tone力度80-100时有~10%概率使用slap采样
        if (tone.velocity >= 80 && tone.velocity <= 100 && (rand() % 100) < 10) {
            engine->triggerSlap(tone.velocity);
        } else {
            engine->trigger(1, tone.velocity);
        }
        hands = 2; toneVel = tone.velocity;
    }
    if (bass.play && bass.velocity > 0 && toneVel == 0) {
        engine->trigger(0, bass.velocity); hands++; bassVel = bass.velocity;
        mLastBassVel = bassVel;
        mLastBassTime = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    if (hands < 2) {
        auto tip = grooveTip(step, e, w);
        if (e <= 0.0f) tip.velocity = 0;
        if (tip.play && tip.velocity > 0) engine->trigger(1, (float)tip.velocity);
    }

    stepWeights[step] = e * w / 4.0f;
    return bassVel;
}

void CajonAssistant::feedNoteOn(double nowMs) {
    std::lock_guard<std::mutex> lock(mEnergyMutex);
    mNoteTimestamps.push_back(nowMs);
}

float CajonAssistant::updateEnergy(double nowMs, double currentBpm) {
    std::lock_guard<std::mutex> lock(mEnergyMutex);
    double cutoff = nowMs - 2000.0;
    mNoteTimestamps.erase(
        std::remove_if(mNoteTimestamps.begin(), mNoteTimestamps.end(),
            [cutoff](double t) { return t < cutoff; }),
        mNoteTimestamps.end());
    double bpmFactor = currentBpm / 70.0;
    if (bpmFactor < 0.5) bpmFactor = 0.5;
    float autoEnergy = std::min(1.0f, (float)(mNoteTimestamps.size() / (24.0 * bpmFactor)));
    setEnergy(autoEnergy);
    return autoEnergy;
}
