#pragma once
#include <atomic>
#include <map>
#include <vector>
#include <string>
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

    // ==== 每子步调用 ====
    void processStep(int step, int bassVel, int toneVel, double nowMs, double bpm,
                     const std::string& chordStr,
                     const std::vector<int>& chordNotes,
                     float energy,
                     AudioEngine& audio, RhythmAudioEngine* rhythmEng);

    /// 从和弦字符串解析贝斯根音, 映射到 A1-#G2 (33-44), 返回 -1 表示无和弦
    static int getBassPitchFromChord(const std::string& chord);

private:
    std::atomic<bool> mEnabled{true};
    std::atomic<float> mVolume{0.8f};

    // 状态
    int mBassNoteOn = -1;
    double mBassNoteTime = 0;
    int mLastBassVel = 100;
    int mPrevBassPitch = -1;
    double mPrevBassTime = 0;
    std::atomic<int> mDampGeneration{0};

    /// 音高 class (0-11) → MIDI A1-#G2 (33-44)
    static int pcToMidiA1_Gs2(int pc);

    /// 从 chordNotes 按概率选音, 概率随能量线性插值
    static std::vector<int> pickNotesFromChord(const std::vector<int>& notes, float energy);

    void playBass(int pitch, int vel, double nowMs, AudioEngine& audio);
    void dampBass(double nowMs, double bpm, const std::vector<int>& chordNotes,
                  float energy, AudioEngine& audio);
};
