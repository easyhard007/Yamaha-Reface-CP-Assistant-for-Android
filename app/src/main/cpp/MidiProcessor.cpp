#include "MidiProcessor.h"
#include <android/log.h>
#include <sstream>

#define TAG "MidiProcessor"

static const char* pitchNames[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

MidiProcessor::MidiProcessor() {}

// ---- Helpers ----

std::set<int> MidiProcessor::getLowNotes() const {
    std::set<int> low;
    for (int n : mActiveNotes)    if (n < mSplitPoint) low.insert(n);
    for (int n : mPedalHeldNotes) if (n < mSplitPoint) low.insert(n);
    return low;
}

std::set<int> MidiProcessor::getAllNotes() const {
    std::set<int> all = mActiveNotes;
    for (int n : mPedalHeldNotes) all.insert(n);
    return all;
}

// ---- MIDI Event Processing ----

NoteOnResult MidiProcessor::processNoteOn(int note, int velocity) {
    std::lock_guard<std::mutex> lock(mLock);
    NoteOnResult r;

    // Auto-sustain collision detection — use lowNotes WITHOUT current note
    auto lowNotes = getLowNotes();
    auto asa = mAutoSustain.onNoteOn(note, lowNotes);

    mActiveNotes.insert(note);
    if (mIsPedalDown) mPedalHeldNotes.insert(note);
    mScaleDetector.feedNote(note);
    if (asa == AutoSustainManager::CC64_OFF) {
        mIsPedalDown = false;
        mPedalHeldNotes.clear();
        mPendingSustainCC = 0;
        r.sustainCCToSend = 0;
        __android_log_print(ANDROID_LOG_INFO, TAG, "[AutoSustain] BREAK → CC64=0");
    }

    return r;
}

NoteOffResult MidiProcessor::processNoteOff(int note) {
    std::lock_guard<std::mutex> lock(mLock);
    NoteOffResult r;

    // Auto-sustain try_repress — BEFORE erasing from ActiveNotes (so note can be re-held)
    auto asa = mAutoSustain.onNoteOff(note);
    if (asa == AutoSustainManager::REHOLD) {
        mIsPedalDown = true;
        for (int n : mActiveNotes) mPedalHeldNotes.insert(n);
        mPendingSustainCC = 127;
        r.sustainCCToSend = 127;
        __android_log_print(ANDROID_LOG_INFO, TAG, "[AutoSustain] REHOLD → CC64=127");
    }

    mActiveNotes.erase(note);

    return r;
}

CCResult MidiProcessor::processCC(int controller, int value) {
    std::lock_guard<std::mutex> lock(mLock);
    CCResult r;

    if (controller == 64 && !mAutoSustain.isEnabled()) {
        // Physical pedal only when auto-sustain is OFF
        if (value >= 64) {
            mIsPedalDown = true;
            for (int n : mActiveNotes) mPedalHeldNotes.insert(n);
        } else {
            mIsPedalDown = false;
            mPedalHeldNotes.clear();
        }
    }

    return r;
}

// ---- Auto-Sustain ----

int MidiProcessor::setAutoSustainEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(mLock);
    auto asa = mAutoSustain.setEnabled(enabled);
    if (asa == AutoSustainManager::CC64_ON) {
        mIsPedalDown = true;
        for (int n : mActiveNotes) mPedalHeldNotes.insert(n);
        mPendingSustainCC = 127;
        __android_log_print(ANDROID_LOG_INFO, TAG, "[AutoSustain] ON → CC64=127");
        return 127;
    } else if (asa == AutoSustainManager::CC64_OFF) {
        mIsPedalDown = false;
        mPedalHeldNotes.clear();
        mPendingSustainCC = 0;
        __android_log_print(ANDROID_LOG_INFO, TAG, "[AutoSustain] OFF → CC64=0");
        return 0;
    }
    return -1;
}

int MidiProcessor::getPendingSustainCC() {
    std::lock_guard<std::mutex> lock(mLock);
    int v = mPendingSustainCC;
    mPendingSustainCC = -1;
    return v;
}

bool MidiProcessor::isAutoSustainEnabled() const {
    return mAutoSustain.isEnabled();
}

// ---- State Accessors ----

int MidiProcessor::changeSplitPoint(int delta) {
    std::lock_guard<std::mutex> lock(mLock);
    mSplitPoint += delta;
    if (mSplitPoint < 21) mSplitPoint = 21;
    if (mSplitPoint > 108) mSplitPoint = 108;
    return mSplitPoint;
}

std::string MidiProcessor::getNoteStateJson() {
    std::lock_guard<std::mutex> lock(mLock);
    auto lowNotes = getLowNotes();
    auto allNotes = getAllNotes();

    auto toJson = [](const std::set<int>& s) -> std::string {
        std::string j;
        bool first = true;
        for (int n : s) {
            if (!first) j += ","; first = false;
            j += std::to_string(n);
        }
        return j;
    };

    return "{\"active\":[" + toJson(mActiveNotes) +
           "],\"pedal\":[" + toJson(mPedalHeldNotes) +
           "],\"low\":[" + toJson(lowNotes) +
           "],\"all\":[" + toJson(allNotes) +
           "],\"pedalDown\":" + std::string(mIsPedalDown ? "true" : "false") +
           ",\"split\":" + std::to_string(mSplitPoint) +
           ",\"autoSustain\":" + std::string(mAutoSustain.isEnabled() ? "true" : "false") +
           ",\"isBreaking\":" + std::string(mAutoSustain.isBreaking() ? "true" : "false") + "}";
}

std::string MidiProcessor::getChordInfo() {
    std::lock_guard<std::mutex> lock(mLock);
    auto allNotes = getAllNotes();

    // Run chord_detect
    std::vector<int> notes(allNotes.begin(), allNotes.end());
    auto chords = detect_chord(notes);

    // Store primary & secondary
    mPrimaryChord = chords.empty() ? "--" : chords[0];
    mSecondaryChord = chords.size() > 1 ? chords[1] : "";

    // Scale detection
    int scaleRoot = mScaleDetector.getScaleRootPc();
    mKeyName = mScaleDetector.getKeyName();

    // Roman numeral from primary chord root
    if (mPrimaryChord != "--" && scaleRoot >= 0) {
        // Extract root pitch class from chord name
        int rootPc = -1;
        std::string rootStr;
        if (mPrimaryChord.size() >= 1) {
            for (int i = 0; i < 12; i++) {
                std::string pn(pitchNames[i]);
                if (mPrimaryChord.compare(0, pn.size(), pn) == 0) {
                    rootPc = i; rootStr = pn; break;
                }
            }
            // Handle two-character names like C#
            if (rootPc < 0 && mPrimaryChord.size() >= 2) {
                for (int i = 0; i < 12; i++) {
                    std::string pn(pitchNames[i]);
                    if (pn.size() == 2 && mPrimaryChord.compare(0, 2, pn) == 0) {
                        rootPc = i; rootStr = pn; break;
                    }
                }
            }
        }

        if (rootPc >= 0) {
            static const char* romanMajor[] = {"I","","ii","","iii","IV","","V","","vi","","viidim"};
            static const char* romanMinor[] = {"i","","iidim","III","","iv","","v","","VI","","viidim"};
            int degree = (rootPc - scaleRoot + 12) % 12;
            mTsdDegree = mScaleDetector.isMinor() ? romanMinor[degree] : romanMajor[degree];
            if (mTsdDegree.empty()) mTsdDegree = mPrimaryChord;

            // Append chord suffix
            std::string suffix = mPrimaryChord.substr(rootStr.size());
            size_t slash = suffix.find('/');
            if (slash != std::string::npos) suffix = suffix.substr(0, slash);
            mRomanNumeral = mTsdDegree + suffix;
        } else {
            mRomanNumeral = mPrimaryChord;
            mTsdDegree = mPrimaryChord;
        }
    } else {
        mRomanNumeral = "--";
        mTsdDegree = "--";
    }

    return mPrimaryChord + "|" + mRomanNumeral + "|" + mKeyName + "|" + mTsdDegree
           + "|" + mSecondaryChord;
}
