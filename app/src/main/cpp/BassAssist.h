#pragma once
#include <atomic>
#include <map>
#include "MidiProcessor.h"  // for NoteInfo

class AudioEngine;
class RhythmAudioEngine;

class BassAssist {
public:
    BassAssist() = default;

    void setEnabled(bool e) { mEnabled.store(e); }
    bool isEnabled() const { return mEnabled.load(); }

    void setVolume(float v) { mVolume.store(v); }
    float getVolume() const { return mVolume.load(); }

    // ==== 每子步调用: 根据 Cajon bass 力度 + 低音集合决定是否弹奏贝斯 ====
    void processStep(int bassVel, double nowMs, const std::map<int, NoteInfo>& lowNotes,
                     AudioEngine& audio, RhythmAudioEngine* rhythmEng);

private:
    std::atomic<bool> mEnabled{true};
    std::atomic<float> mVolume{0.8f};

    // 状态
    int mBassNoteOn = -1;
    double mBassNoteTime = 0;
    int mPrevLowPitch = -1;
    double mPrevLowTime = 0;

    void playBass(int pitch, int vel, double nowMs, AudioEngine& audio);
};
