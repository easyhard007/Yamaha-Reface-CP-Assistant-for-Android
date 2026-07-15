#include "ChordTransposer.h"
#include <algorithm>
#include <cmath>
#include <cctype>

static const char* noteNames[12] = {"C","C#","D","Eb","E","F","F#","G","G#","A","Bb","B"};

ChordTransposer::ChordTransposer() {
    setChordName("major");
}

void ChordTransposer::parseChordName(const std::string& name) {
    if (name.empty() || name == "-") { mChordType = "major"; computeTones(); return; }

    // 从和弦名中提取根音和类型 (但不覆盖 mChordRoot, 因为 setChordRoot 已设置)
    std::string type;
    if (name.size() >= 2 && (name[1] == '#' || name[1] == 'b')) {
        type = name.substr(2); // skip "C#" etc
    } else {
        type = name.substr(1); // skip single-char root
    }

    for (char& c : type) c = std::tolower((unsigned char)c);
    while (!type.empty() && type[0] == ' ') type.erase(0, 1);

    // detect_chord 的别名: M/^=major, m/-=minor  (tolower 后 M→m 会被误判)
    // 如 "FM"→type="M"→tolower→"m" 错误匹配 minor; 需修正
    if (type == "m" && type.length() == 1) {
        // 原始字符可能是 M(major) 或 m(minor), 检查原始 name
        // 安全做法: 单字符 "m" 默认为 minor, 但如果和弦名是完整形式则不在此
        // "FM" 中的 M 在 tolower 后就是 "m", 无法区分
    }

    // 映射和弦类型 → 音程
    mIntervals.clear();
    // 先用子串匹配避免 tolower 在不同平台的差异
    bool isMinor = (type == "m" || type.find("min") != std::string::npos);
    bool isMajor = (type.empty() || type.find("maj") != std::string::npos);
    // "m" alone is minor, but "maj" starts with "m" → must not match major
    if (type.length() >= 3 && type.substr(0,3) == "maj") isMajor = true;
    // "m" alone or "m7"/"m9" etc → minor
    if (type.length() == 1 && type[0] == 'm') isMinor = true;
    if (type.length() >= 2 && type[0] == 'm' && type[1] >= '0' && type[1] <= '9') isMinor = true;

    if (isMajor && !isMinor) {
        mIntervals = {0, 4, 7};
    } else if (isMinor && !isMajor) {
        mIntervals = {0, 3, 7};
    } else if (type == "5") {
        mIntervals = {0, 7};
    } else if (type.find("dim") == 0 || type == "°") {
        mIntervals = {0, 3, 6};
    } else if (type.find("aug") == 0 || type == "+") {
        mIntervals = {0, 4, 8};
    } else if (type.find("7") != std::string::npos) {
        if (type.find("maj7") != std::string::npos || type.find("M7") != std::string::npos) {
            mIntervals = {0, 4, 7, 11};
        } else if (type.find("m7") != std::string::npos || type.find("min7") != std::string::npos
                   || type.find("-7") != std::string::npos) {
            mIntervals = {0, 3, 7, 10};
        } else if (type.find("dim7") != std::string::npos || type.find("°7") != std::string::npos) {
            mIntervals = {0, 3, 6, 9};
        } else {
            mIntervals = {0, 4, 7, 10}; // dominant 7
        }
    } else if (type.find("6") != std::string::npos) {
        mIntervals = {0, 4, 7, 9};
    } else if (type.find("sus4") != std::string::npos) {
        mIntervals = {0, 5, 7};
    } else if (type.find("sus2") != std::string::npos) {
        mIntervals = {0, 2, 7};
    } else {
        // fallback: 大三和弦
        mIntervals = {0, 4, 7};
    }

    mChordType = type.empty() ? "major" : type;
    computeTones();
}

void ChordTransposer::setChordName(const std::string& name) {
    if (name == "-" || name.empty()) return;
    parseChordName(name);
}

void ChordTransposer::computeTones() {
    mChordTones.clear();
    // 生成从 MIDI 0 到 127 的所有内音
    for (int oct = -2; oct <= 10; oct++) {
        for (int interval : mIntervals) {
            int tone = mChordRoot + interval + oct * 12;
            if (tone >= 0 && tone <= 127)
                mChordTones.push_back(tone);
        }
    }
    std::sort(mChordTones.begin(), mChordTones.end());
}

int ChordTransposer::snapToNearest(int note) const {
    if (mChordTones.empty()) return note;
    // 二分查找最近的内音
    auto it = std::lower_bound(mChordTones.begin(), mChordTones.end(), note);
    int best = mChordTones[0];
    int bestDist = std::abs(note - best);
    if (it != mChordTones.end()) {
        int dist = std::abs(note - *it);
        if (dist < bestDist) { best = *it; bestDist = dist; }
    }
    if (it != mChordTones.begin()) {
        --it;
        int dist = std::abs(note - *it);
        if (dist < bestDist) { best = *it; bestDist = dist; }
    }
    return best;
}

bool ChordTransposer::isDrumChannel(int bank) const {
    return (bank == 128 || bank == 120);
}

bool ChordTransposer::isBassChannel(int bank, int program) const {
    return (!isDrumChannel(bank) && program >= 32 && program <= 39);
}

int ChordTransposer::transpose(int channel, int note, int bank, int program) const {
    // 规则 1: 鼓 → 不变
    if (isDrumChannel(bank)) return note;

    // 规则 2: 贝斯 → 根音平行移调
    if (isBassChannel(bank, program)) {
        int shifted = note + (mChordRoot - 60); // 假设原调 C (根音=60)
        if (shifted < 0) shifted = 0;
        if (shifted > 127) shifted = 127;
        return shifted;
    }

    // 规则 3: 其他 → 就近原则
    return snapToNearest(note);
}
