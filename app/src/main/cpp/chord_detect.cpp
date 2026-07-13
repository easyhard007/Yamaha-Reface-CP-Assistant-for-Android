#include "chord_detect.h"
#include <unordered_map>
#include <sstream>
#include <algorithm>
#include <cstdint>

// 提取自 chord-type-data.js
struct RawChordData {
    const char* intervals;
    const char* full_name;
    const char* aliases;
};

static const RawChordData CHORDS_DATA[] = {
    {"1P 3M 5P", "major", "M ^  maj"},
    {"1P 3M 5P 7M", "major seventh", "maj7 Δ ma7 M7 Maj7 ^7"},
    {"1P 3M 5P 7M 9M", "major ninth", "maj9 Δ9 ^9"},
    {"1P 3M 5P 7M 9M 13M", "major thirteenth", "maj13 Maj13 ^13"},
    {"1P 3M 5P 6M", "sixth", "6 add6 add13 M6"},
    {"1P 3M 5P 6M 9M", "sixth added ninth", "6add9 6/9 69 M69"},
    {"1P 3M 6m 7M", "major seventh flat sixth", "M7b6 ^7b6"},
    {"1P 3M 5P 7M 11A", "major seventh sharp eleventh", "maj#4 Δ#4 Δ#11 M7#11 ^7#11 maj7#11"},
    {"1P 3m 5P", "minor", "m min -"},
    {"1P 3m 5P 7m", "minor seventh", "m7 min7 mi7 -7"},
    {"1P 3m 5P 7M", "minor/major seventh", "m/ma7 m/maj7 mM7 mMaj7 m/M7 -Δ7 mΔ -^7 -maj7"},
    {"1P 3m 5P 6M", "minor sixth", "m6 -6"},
    {"1P 3m 5P 7m 9M", "minor ninth", "m9 -9"},
    {"1P 3m 5P 7M 9M", "minor/major ninth", "mM9 mMaj9 -^9"},
    {"1P 3m 5P 7m 9M 11P", "minor eleventh", "m11 -11"},
    {"1P 3m 5P 7m 9M 13M", "minor thirteenth", "m13 -13"},
    {"1P 3m 5d", "diminished", "dim ° o"},
    {"1P 3m 5d 7d", "diminished seventh", "dim7 °7 o7"},
    {"1P 3m 5d 7m", "half-diminished", "m7b5 ø -7b5 h7 h"},
    {"1P 3M 5P 7m", "dominant seventh", "7 dom"},
    {"1P 3M 5P 7m 9M", "dominant ninth", "9"},
    {"1P 3M 5P 7m 9M 13M", "dominant thirteenth", "13"},
    {"1P 3M 5P 7m 11A", "lydian dominant seventh", "7#11 7#4"},
    {"1P 3M 5P 7m 9m", "dominant flat ninth", "7b9"},
    {"1P 3M 5P 7m 9A", "dominant sharp ninth", "7#9"},
    {"1P 3M 7m 9m", "altered", "alt7"},
    {"1P 4P 5P", "suspended fourth", "sus4 sus"},
    {"1P 2M 5P", "suspended second", "sus2"},
    {"1P 4P 5P 7m", "suspended fourth seventh", "7sus4 7sus"},
    {"1P 5P 7m 9M 11P", "eleventh", "11"},
    {"1P 4P 5P 7m 9m", "suspended fourth flat ninth", "b9sus phryg 7b9sus 7b9sus4"},
    {"1P 5P", "fifth", "5"},
    {"1P 3M 5A", "augmented", "aug + +5 ^#5"},
    {"1P 3m 5A", "minor augmented", "m#5 -#5 m+"},
    {"1P 3M 5A 7M", "augmented seventh", "maj7#5 maj7+5 +maj7 ^7#5"},
    {"1P 3M 5P 7M 9M 11A", "major sharp eleventh (lydian)", "maj9#11 Δ9#11 ^9#11"},
    {"1P 2M 4P 5P", "", "sus24 sus4add9"},
    {"1P 3M 5A 7M 9M", "", "maj9#5 Maj9#5"},
    {"1P 3M 5A 7m", "", "7#5 +7 7+ 7aug aug7"},
    {"1P 3M 5A 7m 9A", "", "7#5#9 7#9#5 7alt"},
    {"1P 3M 5A 7m 9M", "", "9#5 9+"},
    {"1P 3M 5A 7m 9M 11A", "", "9#5#11"},
    {"1P 3M 5A 7m 9m", "", "7#5b9 7b9#5"},
    {"1P 3M 5A 7m 9m 11A", "", "7#5b9#11"},
    {"1P 3M 5A 9A", "", "+add#9"},
    {"1P 3M 5A 9M", "", "M#5add9 +add9"},
    {"1P 3M 5P 6M 11A", "", "M6#11 M6b5 6#11 6b5"},
    {"1P 3M 5P 6M 7M 9M", "", "M7add13"},
    {"1P 3M 5P 6M 9M 11A", "", "69#11"},
    {"1P 3m 5P 6M 9M", "", "m69 -69"},
    {"1P 3M 5P 6m 7m", "", "7b6"},
    {"1P 3M 5P 7M 9A 11A", "", "maj7#9#11"},
    {"1P 3M 5P 7M 9M 11A 13M", "", "M13#11 maj13#11 M13+4 M13#4"},
    {"1P 3M 5P 7M 9m", "", "M7b9"},
    {"1P 3M 5P 7m 11A 13m", "", "7#11b13 7b5b13"},
    {"1P 3M 5P 7m 13M", "", "7add6 67 7add13"},
    {"1P 3M 5P 7m 9A 11A", "", "7#9#11 7b5#9 7#9b5"},
    {"1P 3M 5P 7m 9A 11A 13M", "", "13#9#11"},
    {"1P 3M 5P 7m 9A 11A 13m", "", "7#9#11b13"},
    {"1P 3M 5P 7m 9A 13M", "", "13#9"},
    {"1P 3M 5P 7m 9A 13m", "", "7#9b13"},
    {"1P 3M 5P 7m 9M 11A", "", "9#11 9+4 9#4"},
    {"1P 3M 5P 7m 9M 11A 13M", "", "13#11 13+4 13#4"},
    {"1P 3M 5P 7m 9M 11A 13m", "", "9#11b13 9b5b13"},
    {"1P 3M 5P 7m 9m 11A", "", "7b9#11 7b5b9 7b9b5"},
    {"1P 3M 5P 7m 9m 11A 13M", "", "13b9#11"},
    {"1P 3M 5P 7m 9m 11A 13m", "", "7b9b13#11 7b9#11b13 7b5b9b13"},
    {"1P 3M 5P 7m 9m 13M", "", "13b9"},
    {"1P 3M 5P 7m 9m 13m", "", "7b9b13"},
    {"1P 3M 5P 7m 9m 9A", "", "7b9#9"},
    {"1P 3M 5P 9M", "", "Madd9 2 add9 add2"},
    {"1P 3M 5P 9m", "", "Maddb9"},
    {"1P 3M 5d", "", "Mb5"},
    {"1P 3M 5d 6M 7m 9M", "", "13b5"},
    {"1P 3M 5d 7M", "", "M7b5"},
    {"1P 3M 5d 7M 9M", "", "M9b5"},
    {"1P 3M 5d 7m", "", "7b5"},
    {"1P 3M 5d 7m 9M", "", "9b5"},
    {"1P 3M 7m", "", "7no5"},
    {"1P 3M 7m 13m", "", "7b13"},
    {"1P 3M 7m 9M", "", "9no5"},
    {"1P 3M 7m 9M 13M", "", "13no5"},
    {"1P 3M 7m 9M 13m", "", "9b13"},
    {"1P 3m 4P 5P", "", "madd4"},
    {"1P 3m 5P 6m 7M", "", "mMaj7b6"},
    {"1P 3m 5P 6m 7M 9M", "", "mMaj9b6"},
    {"1P 3m 5P 7m 11P", "", "m7add11 m7add4"},
    {"1P 3m 5P 9M", "", "madd9"},
    {"1P 3m 5d 6M 7M", "", "o7M7"},
    {"1P 3m 5d 7M", "", "oM7"},
    {"1P 3m 6m 7M", "", "mb6M7"},
    {"1P 3m 6m 7m", "", "m7#5"},
    {"1P 3m 6m 7m 9M", "", "m9#5"},
    {"1P 3m 5A 7m 9M 11P", "", "m11A"},
    {"1P 3m 6m 9m", "", "mb6b9"},
    {"1P 2M 3m 5d 7m", "", "m9b5"},
    {"1P 4P 5A 7M", "", "M7#5sus4"},
    {"1P 4P 5A 7M 9M", "", "M9#5sus4"},
    {"1P 4P 5A 7m", "", "7#5sus4"},
    {"1P 4P 5P 7M", "", "M7sus4"},
    {"1P 4P 5P 7M 9M", "", "M9sus4"},
    {"1P 4P 5P 7m 9M", "", "9sus4 9sus"},
    {"1P 4P 5P 7m 9M 13M", "", "13sus4 13sus"},
    {"1P 4P 5P 7m 9m 13m", "", "7sus4b9b13 7b9b13sus4"},
    {"1P 4P 7m 10m", "", "4 quartal"},
    {"1P 5P 7m 9m 11P", "", "11b9"}
};

// 内部和弦字典数据结构
struct ChordType {
    std::string name;
    std::vector<std::string> aliases;
    uint16_t chroma;
};

// 分割字符串
static std::vector<std::string> split_string(const std::string& str) {
    std::vector<std::string> result;
    std::istringstream iss(str);
    std::string token;
    while (iss >> token) {
        result.push_back(token);
    }
    return result;
}

// 静态初始化和弦字典
static std::vector<ChordType> build_chord_dict() {
    std::unordered_map<std::string, int> ivl_to_semi = {
        {"1P", 0}, {"2M", 2}, {"3m", 3}, {"3M", 4}, {"4P", 5},
        {"5d", 6}, {"5P", 7}, {"5A", 8}, {"6m", 8}, {"6M", 9},
        {"7d", 9}, {"7m", 10}, {"7M", 11}, {"9m", 1}, {"9M", 2},
        {"9A", 3}, {"10m", 3}, {"11P", 5}, {"11A", 6}, {"13m", 8},
        {"13M", 9}
    };

    std::vector<ChordType> dict;
    for (const auto& raw : CHORDS_DATA) {
        ChordType chord;
        chord.name = raw.full_name;
        chord.aliases = split_string(raw.aliases);

        chord.chroma = 0;
        std::vector<std::string> intervals = split_string(raw.intervals);
        for (const auto& ivl : intervals) {
            int semi = ivl_to_semi[ivl];
            chord.chroma |= (1 << (11 - semi));
        }
        dict.push_back(chord);
    }
    return dict;
}

static const std::vector<ChordType>& get_chord_dict() {
    static std::vector<ChordType> dict = build_chord_dict();
    return dict;
}

static std::string get_default_note_name(int pc) {
    static const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    return names[pc % 12];
}

struct FoundChord {
    float weight;
    std::string name;
};

static const uint16_t BITMASK_ANY_THIRDS = 384;        
static const uint16_t BITMASK_PERFECT_FIFTH = 16;      
static const uint16_t BITMASK_NON_PERFECT_FIFTHS = 40; 
static const uint16_t BITMASK_ANY_SEVENTH = 3;         

// 核心函数实现：注意这里的签名必须和头文件精确一致
std::vector<std::string> detect_chord(const std::vector<int>& midi_notes, DetectOptions options) {
    if (midi_notes.empty()) {
        return {};
    }

    std::vector<int> pcs;
    std::string noteNames[12];
    for (int n : midi_notes) {
        int pc = (n % 12 + 12) % 12;
        if (noteNames[pc].empty()) {
            pcs.push_back(pc);
            noteNames[pc] = get_default_note_name(pc);
        }
    }
    
    int tonicChroma = (midi_notes[0] % 12 + 12) % 12;
    std::string tonicName = noteNames[tonicChroma];

    uint16_t modes[12] = {0};
    for (int i = 0; i < 12; ++i) {
        if (noteNames[i].empty()) continue; 
        uint16_t chroma = 0;
        for (int pc : pcs) {
            int interval = (pc - i + 12) % 12;
            chroma |= (1 << (11 - interval));
        }
        modes[i] = chroma;
    }

    std::vector<FoundChord> found;
    const auto& dict = get_chord_dict();

    for (int i = 0; i < 12; ++i) {
        if (modes[i] == 0) continue;
        
        uint16_t mode = modes[i];
        uint16_t modeWithPF = mode;

        if (options.assumePerfectFifth && !(mode & BITMASK_NON_PERFECT_FIFTHS)) {
            modeWithPF |= BITMASK_PERFECT_FIFTH;
        }

        for (const auto& chord : dict) {
            bool match = false;
            if (options.assumePerfectFifth &&
                (chord.chroma & BITMASK_ANY_THIRDS) &&
                (chord.chroma & BITMASK_PERFECT_FIFTH) &&
                (chord.chroma & BITMASK_ANY_SEVENTH)) 
            {
                match = (chord.chroma == modeWithPF);
            } else {
                match = (chord.chroma == mode);
            }

            if (match) {
                std::string chordName = chord.aliases.empty() ? "" : chord.aliases[0];
                std::string baseNote = noteNames[i];
                bool isInversion = (i != tonicChroma);

                FoundChord fc;
                if (isInversion) {
                    fc.weight = 0.5f; 
                    fc.name = baseNote + chordName + "/" + tonicName;
                } else {
                    fc.weight = 1.0f; 
                    fc.name = baseNote + chordName;
                }
                found.push_back(fc);
            }
        }
    }

    std::stable_sort(found.begin(), found.end(), [](const FoundChord& a, const FoundChord& b) {
        return a.weight > b.weight;
    });

    std::vector<std::string> result;
    for (const auto& fc : found) {
        if (fc.weight > 0) {
            result.push_back(fc.name);
        }
    }

    return result;
}