#pragma once
#include <string>
#include <array>

class ScaleDetector {
public:
    /// Feed a MIDI note (0-127) to accumulate tonal weights. Lower notes get higher weight.
    void feedNote(int midiNote);

    /// Apply time-based decay. Call periodically (e.g. ~100ms intervals).
    void tick(float deltaSeconds);

    /// Get the pitch class (0-11) of the detected scale root, or -1 if uncertain.
    int getScaleRootPc() const;

    /// Returns "C大调" / "Am小调" / "--"
    std::string getKeyName() const;

    /// Returns true if the detected key is minor.
    bool isMinor() const { getScaleRootPc(); return mCachedIsMinor; }

    /// Get raw weights for debugging
    const std::array<float, 12>& getWeights() const { return mWeights; }

private:
    std::array<float, 12> mWeights = {};
    mutable int mCachedRootPc = -1;
    mutable bool mCachedIsMinor = false;
    mutable float mCachedTotalWeight = 0.0f;
    mutable int mRecalcCounter = 0; // recalc every N feedNote calls

    void recalcScale() const;
};
