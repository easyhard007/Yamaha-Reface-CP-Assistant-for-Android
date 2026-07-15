#include "LowChordDetector.h"
#include "chord_detect.h"
#include <vector>
#include <algorithm>
#include <chrono>
#include <sstream>
#include <iomanip>

bool LowChordDetector::samePitchClass(int a, int b) {
    return (a % 12) == (b % 12);
}

bool LowChordDetector::chordContainsPitchClass(const std::set<int>& notes, int note) {
    int pc = note % 12;
    for (int n : notes) if ((n % 12) == pc) return true;
    return false;
}

static const char* noteNames[12] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};

static std::string rootName(const std::set<int>& notes) {
    if (notes.empty()) return "?";
    return noteNames[*notes.begin() % 12];
}

static std::string fullChordName(const std::set<int>& notes, const std::string& type) {
    if (type.empty() || type == "-") return "-";
    // 标准化别名: "FM" / "Fm" / "Fmaj" → "Fmajor" / "Fminor"
    std::string norm = type;
    // 检查是否已经包含根音
    bool hasRoot = false;
    for (int i = 0; i < 12; i++) {
        if (norm.find(noteNames[i]) == 0) { hasRoot = true; break; }
    }
    if (!hasRoot) return rootName(notes) + norm;
    // 提取根音后的别名并展开
    std::string rt, alias;
    if (norm.size() >= 2 && (norm[1] == '#' || norm[1] == 'b')) {
        rt = norm.substr(0, 2); alias = norm.substr(2);
    } else {
        rt = norm.substr(0, 1); alias = norm.substr(1);
    }
    // 展开已知别名
    if (alias == "M" || alias == "^" || alias == "maj") alias = "major";
    else if (alias == "m" || alias == "-" || alias == "min") alias = "minor";
    else if (alias == "dim" || alias.empty()) alias = (alias.empty() ? "major" : alias);
    return rt + alias;
}

void LowChordDetector::feedNotes(const std::set<int>& lowNotes,
                                  const std::set<int>& allNotes,
                                  int splitNote) {
    using namespace std::chrono;
    auto t0 = steady_clock::now();
    std::ostringstream timing;
    timing << std::fixed << std::setprecision(1);
    int detectCalls = 0;

    auto timeDetect = [&](const char* label) {
        auto t = steady_clock::now();
        double us = duration_cast<microseconds>(t - t0).count();
        if (detectCalls > 0) timing << "|";
        timing << label << ":" << (int)us << "us";
        detectCalls++;
    };

    if (lowNotes.empty()) { mTiming = "empty"; return; }

    // 根音 = 低音区最低音
    int root = *lowNotes.begin();
    mRootMidi = root;

    // 单音和弦: 直接视为 X5
    if (lowNotes.size() == 1) {
        std::string newChord = std::string(noteNames[root % 12]) + "5";
        timeDetect("1note");
        std::lock_guard<std::mutex> lock(mLock);
        if (mChord != "-" && mLevel > 1) {
            std::string newRoot = rootName(lowNotes);
            if (mChord.compare(0, newRoot.size(), newRoot) == 0) {
                if (mChord.size() == newRoot.size() ||
                    (mChord[newRoot.size()] != '#' && mChord[newRoot.size()] != 'b'))
                { mTiming = timing.str(); return; }
            }
        }
        mChord = newChord;
        mLevel = 1;
        mRootMidi = root;
        mTiming = timing.str();
        return;
    }

    // 工作集合: lowNotes 副本
    std::set<int> working(lowNotes.begin(), lowNotes.end());

    // 候选音: allNotes - lowNotes (即右手区域), 从低到高
    std::vector<int> candidates;
    for (int n : allNotes) {
        if (n >= splitNote && working.find(n) == working.end()
            && working.find(n - 12) == working.end()
            && working.find(n + 12) == working.end()) {
            bool dup = false;
            for (int w : working) if (samePitchClass(w, n)) { dup = true; break; }
            if (!dup) candidates.push_back(n);
        }
    }
    std::sort(candidates.begin(), candidates.end());

    std::vector<int> wv(working.begin(), working.end());
    auto results = detect_chord(wv);
    timeDetect("raw");

    if (results.empty()) {
        std::lock_guard<std::mutex> lock(mLock);
        if (mChord == "-" || mLevel == 0) { mLevel = 1; mChord = "-"; }
        for (int c : candidates) {
            std::set<int> test = working; test.insert(c);
            std::vector<int> tv(test.begin(), test.end());
            auto r2 = detect_chord(tv);
            timeDetect("enr1");
            if (!r2.empty()) {
                mChord = fullChordName(test, r2[0]); mLevel = 2;
                for (int c2 : candidates) {
                    if (c2 == c || chordContainsPitchClass(test, c2)) continue;
                    std::set<int> test2 = test; test2.insert(c2);
                    std::vector<int> tv2(test2.begin(), test2.end());
                    auto r3 = detect_chord(tv2);
                    if (!r3.empty()) {
                        timeDetect("enr2");
                        std::string newName = fullChordName(test2, r3[0]);
                        if (newName != mChord) { mChord = newName; mLevel = 3; mTiming = timing.str(); return; }
                    }
                }
                mTiming = timing.str(); return;
            }
        }
        mTiming = timing.str(); return;
    }

    {
        std::lock_guard<std::mutex> lock(mLock);
        mChord = fullChordName(working, results[0]); mLevel = 2;
    }

    std::string prevChord = mChord;
    for (int c : candidates) {
        if (chordContainsPitchClass(working, c)) continue;
        std::set<int> test = working; test.insert(c);
        std::vector<int> tv(test.begin(), test.end());
        auto r2 = detect_chord(tv);
        timeDetect("upg");
        if (!r2.empty()) {
            std::string newName = fullChordName(test, r2[0]);
            if (newName != prevChord) {
                std::lock_guard<std::mutex> lock(mLock);
                mChord = newName; mLevel = 3; mTiming = timing.str(); return;
            }
        }
    }
    mTiming = timing.str();
}

std::string LowChordDetector::getChord() const {
    std::lock_guard<std::mutex> lock(mLock);
    return mChord;
}

int LowChordDetector::getRootMidi() const {
    std::lock_guard<std::mutex> lock(mLock);
    return mRootMidi;
}

int LowChordDetector::getLevel() const {
    std::lock_guard<std::mutex> lock(mLock);
    return mLevel;
}

std::string LowChordDetector::getTiming() const {
    std::lock_guard<std::mutex> lock(mLock);
    return mTiming;
}

void LowChordDetector::reset() {
    std::lock_guard<std::mutex> lock(mLock);
    mChord = "-";
    mRootMidi = 60;
    mLevel = 0;
    mTiming.clear();
}
