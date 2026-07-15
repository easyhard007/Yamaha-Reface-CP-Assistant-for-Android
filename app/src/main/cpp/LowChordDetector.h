#pragma once
#include <string>
#include <set>
#include <mutex>

class LowChordDetector {
public:
    /// 用 lowNotes + allNotes 检测和弦, 应用和弦升级规则
    void feedNotes(const std::set<int>& lowNotes,
                   const std::set<int>& allNotes,
                   int splitNote);

    std::string getChord() const;
    int         getRootMidi() const; // 和弦根音的 MIDI 音高
    int         getLevel() const;
    std::string getTiming() const;
    void        reset();

private:
    mutable std::mutex mLock;
    std::string mChord = "-";
    int         mRootMidi = 60;
    int         mLevel = 0;
    std::string mTiming;

    static bool samePitchClass(int a, int b); // 同音名 (模 12)
    static bool chordContainsPitchClass(const std::set<int>& notes, int note);
};
