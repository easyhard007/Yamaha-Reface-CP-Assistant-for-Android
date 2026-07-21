#pragma once
#include <string>
#include <vector>
#include <set>
#include <cmath>

class ChordDetector {
public:
    ChordDetector();

    /// 输入所有发声音符 (Active ∪ Pedal), 返回识别的和弦名
    std::string detect(const std::set<int>& allNotes);

    /// 获取当前和弦名 ("" = 无)
    std::string getChord() const { return mChord; }

    /// 获取和弦根音 MIDI (0-127, -1 = 无)
    int getRootMidi() const;

    /// 获取和弦类型 (major, minor, 5, etc.)
    std::string getChordType() const { return mChordType; }

    /// 和弦是否发生变化 (调用 detect 后检查)
    bool changed() const { return mChanged; }

private:
    std::string mChord;
    std::string mChordType;
    int mRootPc = -1;
    int mBassPc = -1;
    bool mChanged = false;

    static constexpr double DECAY_RATE = 0.03;
    static constexpr double BASS_BONUS = 2.0;

    struct ChordTemplate {
        const char* type;
        const char* name;
        std::vector<int> intervals;
    };
    static const std::vector<ChordTemplate> TEMPLATES;
    static const char* NOTE_NAMES[12];
};
