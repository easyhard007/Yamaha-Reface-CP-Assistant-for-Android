#include "BassAssist.h"
#include "AudioEngine.h"
#include "RhythmAudioEngine.h"
#include <cmath>
#include <android/log.h>

void BassAssist::playBass(int pitch, int vel, double nowMs, AudioEngine& audio) {
    int bp = pitch;
    while (bp > 42) bp -= 12;
    while (bp < 31) bp += 12;
    if (mBassNoteOn >= 0) audio.enqueueNoteOff(2, 0, mBassNoteOn);
    audio.enqueueNoteOn(2, 0, bp, vel);
    mBassNoteOn = bp;
    mBassNoteTime = nowMs;
}

void BassAssist::processStep(int bassVel, double nowMs, const std::map<int, NoteInfo>& lowNotes,
                              AudioEngine& audio, RhythmAudioEngine* rhythmEng) {
    // 找最低音
    int lowPitch = -1, lowVel = 0;
    double lowTime = 0;
    for (const auto& kv : lowNotes) {
        if (lowPitch < 0 || kv.first < lowPitch) {
            lowPitch = kv.first; lowVel = kv.second.velocity; lowTime = kv.second.timestampMs;
        }
    }

    if (!mEnabled.load()) {
        mPrevLowPitch = lowPitch;
        mPrevLowTime = lowTime;
        return;
    }

    // 场景 1: Cajon bass 触发 (>45)
    if (bassVel > 45 && lowPitch > 0) {
        // float curve = std::pow(bassVel / 127.0f, 0.6f);
        // int vel = (int)(curve * 127.0f * 1.5f);
        int vel = 100;  // 固定力度
        __android_log_print(ANDROID_LOG_INFO, "BassAssist", "trigger bassVel=%d vel=%d pitch=%d", bassVel, vel, lowPitch);
        if (vel > 1) playBass(lowPitch, vel, nowMs, audio);
    }
    // 场景 2: 无低音 → 有低音, 且 100ms 内有 Cajon bass (lastBassVel/Time 由外部记录)
    // (此场景依赖 CajonAssistant, 仍在 native-lib 中处理)
    // 场景 3: 最低音变低
    if (mPrevLowPitch > 0 && lowPitch > 0 && lowPitch < mPrevLowPitch && mBassNoteOn >= 0) {
        double elapsed = nowMs - mPrevLowTime;
        if (elapsed < 3200.0) {
            int vel = (int)((3200.0 - elapsed) / 3200.0 * 127.0);
            if (vel > 0) playBass(lowPitch, vel, nowMs, audio);
        }
    }
    mPrevLowPitch = lowPitch;
    mPrevLowTime = lowTime;

    // 10 秒超时自动 note-off
    if (mBassNoteOn >= 0 && (nowMs - mBassNoteTime) > 10000.0) {
        audio.enqueueNoteOff(2, 0, mBassNoteOn);
        mBassNoteOn = -1;
    }
}
