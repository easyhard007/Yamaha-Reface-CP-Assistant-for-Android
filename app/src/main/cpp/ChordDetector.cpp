#include "ChordDetector.h"
#include <algorithm>
#include <cmath>

static const char* PITCH_NAMES[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

const std::vector<ChordDetector::ChordTemplate> ChordDetector::sTemplates = {
    // Triads
    {"",      "",    {0,4,7},       10},  // Major
    {"m",     "m",   {0,3,7},       10},  // Minor
    {"dim",   "dim", {0,3,6},        8},  // Diminished
    {"aug",   "aug", {0,4,8},        7},  // Augmented
    {"sus4",  "sus", {0,5,7},        6},  // Sus4
    {"sus2",  "sus", {0,2,7},        5},  // Sus2
    {"5",     "",    {0,7},          4},  // Power chord

    // Sevenths
    {"7",     "7",   {0,4,7,10},     9},  // Dominant 7
    {"m7",    "m7",  {0,3,7,10},     9},  // Minor 7
    {"maj7",  "maj7",{0,4,7,11},     9},  // Major 7
    {"m7b5",  "m7b5",{0,3,6,10},     7},  // Half-diminished
    {"dim7",  "dim7",{0,3,6,9},      7},  // Diminished 7
};

static std::string noteName(int pc) {
    return PITCH_NAMES[pc % 12];
}

ChordResult ChordDetector::detect(const std::set<int>& midiNotes) {
    ChordResult result;

    if (midiNotes.empty()) return result;

    int bassNote = *midiNotes.begin();  // lowest MIDI note
    int bassPc = bassNote % 12;

    // Collect unique pitch classes
    std::vector<int> pcs;
    for (int n : midiNotes) {
        int pc = n % 12;
        if (pcs.empty() || pc != pcs.back()) {
            pcs.push_back(pc);
        }
    }

    if (pcs.size() == 1) {
        result.chordName = noteName(pcs[0]);
        result.rootPc = pcs[0];
        return result;
    }

    // ---- Try each pitch class as root, score against templates ----
    int bestRoot = -1;
    int bestTemplateIdx = -1;
    float bestScore = -1.0f;

    for (int rootPc : pcs) {
        // Compute intervals from this candidate root
        std::vector<int> intervals;
        for (int pc : pcs) {
            int iv = (pc - rootPc + 12) % 12;
            if (intervals.empty() || iv != intervals.back()) {
                intervals.push_back(iv);
            }
        }

        for (size_t ti = 0; ti < sTemplates.size(); ti++) {
            const auto& tmpl = sTemplates[ti];

            // Count matched intervals
            int matched = 0;
            for (int tiv : tmpl.intervals) {
                for (int iv : intervals) {
                    if (iv == tiv) { matched++; break; }
                }
            }

            // Score: coverage ratio * template weight, penalize extra notes
            float coverage = (float)matched / tmpl.intervals.size();
            int extra = (int)intervals.size() - matched;
            float score = coverage * tmpl.weight - extra * 2.0f;

            // Bonus: if root is the bass, prefer it
            if (rootPc == bassPc) score += 1.0f;

            if (score > bestScore) {
                bestScore = score;
                bestRoot = rootPc;
                bestTemplateIdx = (int)ti;
            }
        }
    }

    if (bestRoot < 0 || bestTemplateIdx < 0) {
        result.chordName = noteName(bassPc) + "?";
        result.rootPc = bassPc;
        return result;
    }

    const auto& tmpl = sTemplates[bestTemplateIdx];
    result.rootPc = bestRoot;
    result.chordName = noteName(bestRoot) + tmpl.name;

    // Inversion: root ≠ bass
    if (bestRoot != bassPc) {
        result.chordName += "/" + noteName(bassPc);
        result.hasInversion = true;
    }

    return result;
}
