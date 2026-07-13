#pragma once
#include <set>
#include <string>
#include <mutex>
#include <vector>
#include "chord_detect.h"
#include "ScaleDetector.h"
#include "AutoSustainManager.h"

/// Returned by processNoteOn — tells the caller what to do
struct NoteOnResult {
    bool shouldPlay = true;       // send to FluidLite?
    bool shouldSendToDevice = false; // forward to external MIDI?
    int sustainCCToSend = -1;     // -1=none, 0=off, 127=on
};

struct NoteOffResult {
    bool shouldStop = true;       // send note-off to FluidLite?
    int sustainCCToSend = -1;
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

    // ---- State accessors (for polling/JNI) ----
    std::string getNoteStateJson();
    std::string getChordInfo();
    int  changeSplitPoint(int delta);
    int  getSplitPoint() const { return mSplitPoint; }

private:
    mutable std::mutex mLock;

    // Note state
    std::set<int> mActiveNotes;
    std::set<int> mPedalHeldNotes;

    // Pedal
    bool mIsPedalDown = false;

    // Split point
    int mSplitPoint = 52; // E3

    // Auto-sustain
    AutoSustainManager mAutoSustain;
    int mPendingSustainCC = -1;

    // Analysis
    ScaleDetector mScaleDetector;
    std::string mPrimaryChord = "--";
    std::string mSecondaryChord = "";
    std::string mRomanNumeral = "--";
    std::string mKeyName = "--";
    std::string mTsdDegree = "--";
    std::string mCachedChordInfo;

    // Helpers
    std::set<int> getLowNotes() const; // Active ∪ PedalHeld ∩ (< splitPoint)
    std::set<int> getAllNotes() const; // Active ∪ PedalHeld
};
