#pragma once
#include <string>
#include <set>
#include <vector>

struct ChordResult {
    std::string chordName;   // e.g. "Cmaj7", "Dm", "G7/B", "C"
    int rootPc = -1;         // root pitch class 0-11, -1 if unknown
    bool hasInversion = false;
};

class ChordDetector {
public:
    /// Detect chord from sorted MIDI notes (0-127). Returns empty name if < 2 notes.
    static ChordResult detect(const std::set<int>& midiNotes);

private:
    struct ChordTemplate {
        const char* name;
        const char* romanSuffix; // appended to roman numeral, e.g. "", "m", "7", "m7", "dim"
        std::vector<int> intervals;
        int weight = 1;          // higher = preferred tiebreaker
    };

    static const std::vector<ChordTemplate> sTemplates;
};
