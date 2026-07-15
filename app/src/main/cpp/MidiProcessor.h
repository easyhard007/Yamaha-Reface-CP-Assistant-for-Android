#pragma once
#include <set>
#include <string>
#include <mutex>
#include <vector>
#include "chord_detect.h"
#include "BassEnhancer.h"
#include "ScaleDetector.h"
#include "AutoSustainManager.h"

/// Returned by processNoteOn — tells the caller what to do
struct NoteOnResult {
    bool shouldPlay = true;
    int sustainCCToSend = -1;     // -1=none, 0=off, 127=on
    int bassNote = -1;            // bass note to send (-12 octave), -1 = none
    int bassVelocity = 0;
};

struct NoteOffResult {
    bool shouldStop = true;
    int sustainCCToSend = -1;
    int bassNote = -1;            // bass note-off (-12 octave), -1 = none
};

struct CCResult {
    int sustainCCToSend = -1;
};

/// Central MIDI processing — owns all note/pedal/sustain state
/// Independent of AudioEngine. Call from MyMidiReceiver.
class MidiProcessor {
public:
    MidiProcessor();

    // ---- MIDI event processing ----
    NoteOnResult processNoteOn(int note, int velocity);
    NoteOffResult processNoteOff(int note);
    CCResult      processCC(int controller, int value);

    // ---- Auto-sustain ----
    int  setAutoSustainEnabled(bool enabled); // returns CC64 to send (-1/0/127)
    int  getPendingSustainCC();               // clears after read
    bool isAutoSustainEnabled() const;

    // ---- Bass enhance ----
    void setBassEnhanceEnabled(bool enabled);
    bool isBassEnhanceEnabled() const { return mBassEnhanceEnabled; }
    float getBassWeight(int note) const;  // weight for a given MIDI note

    // ---- State accessors (for polling/JNI) ----
    std::string getNoteStateJson();
    std::string getChordInfo();
    int  changeSplitPoint(int delta);
    int  getSplitPoint() const { return mSplitPoint; }
    int  changeTranspose(int delta);  // returns new value
    int  getTranspose() const { return mTranspose; }

private:
    mutable std::mutex mLock;

    // Note state
    std::set<int> mActiveNotes;
    std::set<int> mPedalHeldNotes;

    // Pedal
    bool mIsPedalDown = false;

    // Split point
    int mSplitPoint = 52; // E3
    int mTranspose = 0;        // 升降调 (-12 ~ +12)

    // Auto-sustain
    AutoSustainManager mAutoSustain;
    int mPendingSustainCC = -1;

    // Bass enhance
    BassEnhancer mBassEnhancer;
    bool mBassEnhanceEnabled = false;
    float mBassEnhanceRatio = 0.5f;
    int mBassEnhanceCenter = 43;
    int mBassEnhanceSpread = 12;
    float mBassWeights[128] = {};
    bool mBassWeightsDirty = true;
    void recomputeBassWeights();

    // Analysis
    ScaleDetector mScaleDetector;
    std::string mPrimaryChord = "--";
    std::string mSecondaryChord = "";
    std::string mRomanNumeral = "--";
    std::string mKeyName = "--";
    std::string mTsdDegree = "--";
    std::string mCachedChordInfo;

    // Helpers
public:
    std::set<int> getLowNotes() const;  // Active ∪ PedalHeld ∩ (< splitPoint)
    std::set<int> getAllNotes() const;  // Active ∪ PedalHeld
    int         getSplitNote() const { return mSplitPoint; }
};
