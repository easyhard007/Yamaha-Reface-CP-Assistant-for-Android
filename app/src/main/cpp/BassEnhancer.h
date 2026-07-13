#pragma once
#include <cstdint>

struct BassNote {
    int note;
    int velocity;
    bool noteOn;  // true = note on, false = note off
};

/// Simple bass enhancer: sends lower octave (-12) with weighted velocity
class BassEnhancer {
public:
    void setEnabled(bool enabled) { mEnabled = enabled; }
    bool isEnabled() const { return mEnabled; }

    /// Process note on — returns bass note-on if weight > 0
    BassNote processNoteOn(int note, int velocity, const float weights[128]) const;

    /// Process note off — returns bass note-off regardless of weight
    BassNote processNoteOff(int note) const;

private:
    bool mEnabled = false;
};
