#include "ChordDetector.h"
#include <algorithm>
#include <numeric>

ChordDetector::ChordDetector() : mChord("-") {}

const char* ChordDetector::NOTE_NAMES[12] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};

const std::vector<ChordDetector::ChordTemplate> ChordDetector::TEMPLATES = {
    {"5", "五和弦", {0, 7}},
    {"", "大三", {0, 4, 7}},
    {"m", "小三", {0, 3, 7}},
    {"dim", "减三", {0, 3, 6}},
    {"aug", "增三", {0, 4, 8}},
    {"sus4", "挂四", {0, 5, 7}},
    {"sus2", "挂二", {0, 2, 7}},
    {"7", "属七", {0, 4, 7, 10}},
    {"maj7", "大七", {0, 4, 7, 11}},
    {"m7", "小七", {0, 3, 7, 10}},
    {"m7b5", "半减七", {0, 3, 6, 10}},
    {"dim7", "减七", {0, 3, 6, 9}},
    {"maj9", "大九", {0, 4, 7, 11, 2}},
    {"m9", "小九", {0, 3, 7, 10, 2}},
    {"9", "属九", {0, 4, 7, 10, 2}},
    {"7b9", "属七降九", {0, 4, 7, 10, 1}},
    {"7#9", "属七增九", {0, 4, 7, 10, 3}},
};

int ChordDetector::getRootMidi() const {
    if (mRootPc < 0 || mBassPc < 0) return -1;
    // 根音 MIDI = bass octave base + root pitch class relative to bass
    // 简单返回根音 PC + 48 (C3 区域)
    for (int midi = mBassPc; midi <= 96; midi += 12) {
        if (midi % 12 == mRootPc) {
            // 找最接近 bass 的那个 root
            int candidate = midi;
            int diff = candidate - mBassPc;
            if (diff < 0) diff += 12;
            if (diff <= 11) return candidate;
        }
    }
    return mRootPc + 48; // fallback
}

std::string ChordDetector::detect(const std::set<int>& allNotes) {
    std::string prev = mChord;

    if (allNotes.empty()) {
        mChord = "-";
        mRootPc = -1; mBassPc = -1;
        mChordType = "";
        mChordNotes.clear();
        mChanged = (prev != mChord);
        return mChord;
    }

    std::vector<int> notes(allNotes.begin(), allNotes.end());
    std::sort(notes.begin(), notes.end());
    int bassNote = notes[0];
    int bassPC = bassNote % 12;
    mBassPc = bassNote; // store actual MIDI note

    // Build weighted chroma vector
    double chroma[12] = {};
    for (int note : notes) {
        int pc = note % 12;
        double distance = (double)(note - bassNote);
        double weight = std::exp(-DECAY_RATE * distance);
        if (note == bassNote) weight *= BASS_BONUS;
        chroma[pc] += weight;
    }

    // Single note → "X5" (五和弦)
    if (notes.size() < 2) {
        mChord = std::string(NOTE_NAMES[bassPC]) + "5";
        mChordType = "5";
        mRootPc = bassPC;
        computeChordNotes();
        mChanged = (prev != mChord);
        return mChord;
    }

    // Template matching
    double totalWeight = 0;
    for (int i = 0; i < 12; i++) totalWeight += chroma[i];

    double bestScore = -999;
    std::string bestName = "-";
    int bestRoot = 0;
    std::string bestType;

    for (int root = 0; root < 12; root++) {
        for (const auto& tmpl : TEMPLATES) {
            double matched = 0;
            int missing = 0;
            for (int interval : tmpl.intervals) {
                int pc = (root + interval) % 12;
                if (chroma[pc] < 0.1) missing++;
                matched += chroma[pc];
            }
            double unexpected = totalWeight - matched;
            double score = matched - unexpected * 2.0 - std::pow((double)missing, 1.5) * 1.2;

            // Bass bonus
            if (bassPC == root) score *= 1.1;
            else {
                bool bassInChord = false;
                for (int interval : tmpl.intervals) {
                    if ((root + interval) % 12 == bassPC) { bassInChord = true; break; }
                }
                if (!bassInChord) score *= 0.4;
            }

            if (score > bestScore) {
                bestScore = score;
                bestRoot = root;
                bestType = tmpl.type;
                bestName = std::string(NOTE_NAMES[root]) + tmpl.type;
                if (bassPC != root) bestName += "/" + std::string(NOTE_NAMES[bassPC]);
            }
        }
    }

    if (bestScore > 0.1) {
        mChord = bestName;
        mChordType = bestType;
        mRootPc = bestRoot;
    } else {
        mChord = "-";
        mChordType = "";
        mRootPc = -1;
    }
    mChanged = (prev != mChord);
    computeChordNotes();
    return mChord;
}

// ---- 音符名 → 音高 class (0-11) ----
static int noteNameToPc(const std::string& name) {
    if (name.empty()) return -1;
    char root = name[0];
    int semi = -1;
    switch (root) {
        case 'C': semi = 0; break; case 'D': semi = 2; break;
        case 'E': semi = 4; break; case 'F': semi = 5; break;
        case 'G': semi = 7; break; case 'A': semi = 9; break;
        case 'B': semi = 11; break;
        default: return -1;
    }
    if (name.size() > 1) {
        if (name[1] == '#') semi++;
        else if (name[1] == 'b') semi--;
    }
    while (semi < 0) semi += 12;
    return semi % 12;
}

void ChordDetector::computeChordNotes() {
    mChordNotes.clear();
    if (mChord == "-" || mChord.empty()) return;

    // 解析: 找 "/" 分离 bass note
    std::string chordPart = mChord;
    std::string bassName;
    size_t slashPos = chordPart.find('/');
    if (slashPos != std::string::npos) {
        bassName = chordPart.substr(slashPos + 1);
        chordPart = chordPart.substr(0, slashPos);
    }

    // 提取根音名 (1-2字符)
    std::string rootName = chordPart.substr(0, 1);
    if (chordPart.size() > 1 && (chordPart[1] == '#' || chordPart[1] == 'b'))
        rootName += chordPart[1];

    // 提取和弦类型
    std::string type = chordPart.substr(rootName.size());

    int rootPC = noteNameToPc(rootName);
    int bassPC = rootPC;
    if (!bassName.empty()) bassPC = noteNameToPc(bassName);
    if (bassPC < 0 || rootPC < 0) return;

    // 1. 先放 bass note
    mChordNotes.push_back(bassPC);

    // 2. 再放 root (如果 != bass)
    if (rootPC != bassPC) mChordNotes.push_back(rootPC);

    // 3. 从模板取间隔, 添加其他组成音
    for (const auto& tmpl : TEMPLATES) {
        if (tmpl.type == type) {
            for (int interval : tmpl.intervals) {
                if (interval == 0) continue; // 根音已处理
                int pc = (rootPC + interval) % 12;
                if (std::find(mChordNotes.begin(), mChordNotes.end(), pc) == mChordNotes.end())
                    mChordNotes.push_back(pc);
            }
            break;
        }
    }
}

std::string ChordDetector::getChordNotesString() const {
    if (mChordNotes.empty()) return "";
    std::string result = "[";
    for (size_t i = 0; i < mChordNotes.size(); i++) {
        if (i > 0) result += ", ";
        result += NOTE_NAMES[mChordNotes[i]];
    }
    result += "]";
    return result;
}
