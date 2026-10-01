#include "MidiProcessor.h"
#include <android/log.h>
#include <sstream>
#include <cmath>

#define TAG "MidiProcessor"

void MidiProcessor::setBassEnhanceEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(mLock);
    mBassEnhanceEnabled = enabled;
    mBassEnhancer.setEnabled(enabled);
}

float MidiProcessor::getBassWeight(int note) const {
    if (!mBassEnhanceEnabled || note < 0 || note > 127) return 0;
    if (mBassWeightsDirty) const_cast<MidiProcessor*>(this)->recomputeBassWeights();
    return mBassWeights[note];
}

void MidiProcessor::recomputeBassWeights() {
    float variance2 = (mBassEnhanceSpread * mBassEnhanceSpread) / logf(10.0f);
    if (variance2 < 0.001f) variance2 = 0.001f;
    for (int i = 0; i < 128; i++) {
        float x = (float)(i - mBassEnhanceCenter);
        float w = expf(-(x * x) / variance2) * mBassEnhanceRatio;
        mBassWeights[i] = (w >= 0.1f * mBassEnhanceRatio) ? w : 0.0f;
    }
    mBassWeightsDirty = false;
}

static const char* pitchNames[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

MidiProcessor::MidiProcessor() {}

// ---- Helpers ----

std::map<int, NoteInfo> MidiProcessor::getLowNotes() const {
    std::map<int, NoteInfo> low;
    for (const auto& kv : mActiveNotes) if (kv.first < mSplitPoint) low.insert(kv);
    for (const auto& kv : mPedalHeldNotes) if (kv.first < mSplitPoint) low.insert(kv);
    return low;
}

std::set<int> MidiProcessor::getAllNotes() const {
    std::set<int> all;
    for (const auto& kv : mActiveNotes) all.insert(kv.first);
    for (const auto& kv : mPedalHeldNotes) all.insert(kv.first);
    return all;
}

MidiNoteSetSnapshot MidiProcessor::getNoteSetSnapshot() const {
    std::lock_guard<std::mutex> lock(mLock);
    MidiNoteSetSnapshot snapshot;
    for (const auto& kv : mActiveNotes) {
        snapshot.allNotes.insert(kv.first);
        if (kv.first < mSplitPoint) snapshot.lowNotes.insert(kv.first);
    }
    for (const auto& kv : mPedalHeldNotes) {
        snapshot.allNotes.insert(kv.first);
        if (kv.first < mSplitPoint) snapshot.lowNotes.insert(kv.first);
    }
    return snapshot;
}

// ---- MIDI Event Processing ----

NoteOnResult MidiProcessor::processNoteOn(int note, int velocity, double timestampMs) {
    std::lock_guard<std::mutex> lock(mLock);
    NoteOnResult r;

    auto lowNotes = getLowNotes();
    std::set<int> lowPitches;
    for (const auto& kv : lowNotes) lowPitches.insert(kv.first);
    auto asa = mAutoSustain.onNoteOn(note, lowPitches);

    mActiveNotes[note] = {note, velocity, timestampMs};
    if (mIsPedalDown) mPedalHeldNotes[note] = {note, velocity, timestampMs};
    mScaleDetector.feedNote(note);
    if (asa == AutoSustainManager::CC64_OFF) {
        mIsPedalDown = false;
        mPedalHeldNotes.clear();
        mPendingSustainCC = 0;
        r.sustainCCToSend = 0;
        __android_log_print(ANDROID_LOG_INFO, TAG, "[AutoSustain] BREAK → CC64=0");
    }

    if (mBassWeightsDirty) recomputeBassWeights();
    BassNote bn = mBassEnhancer.processNoteOn(note, velocity, mBassWeights);
    if (bn.noteOn) { r.bassNote = bn.note; r.bassVelocity = bn.velocity; }

    return r;
}

NoteOffResult MidiProcessor::processNoteOff(int note) {
    std::lock_guard<std::mutex> lock(mLock);
    NoteOffResult r;

    auto asa = mAutoSustain.onNoteOff(note);
    if (asa == AutoSustainManager::REHOLD) {
        mIsPedalDown = true;
        for (const auto& kv : mActiveNotes) mPedalHeldNotes.insert(kv);
        mPendingSustainCC = 127;
        r.sustainCCToSend = 127;
        __android_log_print(ANDROID_LOG_INFO, TAG, "[AutoSustain] REHOLD → CC64=127");
    }

    mActiveNotes.erase(note);

    BassNote bnOff = mBassEnhancer.processNoteOff(note);
    if (mBassEnhancer.isEnabled()) { r.bassNote = bnOff.note; }

    return r;
}

CCResult MidiProcessor::processCC(int controller, int value) {
    std::lock_guard<std::mutex> lock(mLock);
    CCResult r;

    if (controller == 64 && !mAutoSustain.isEnabled()) {
        if (value >= 64) {
            mIsPedalDown = true;
            for (const auto& kv : mActiveNotes) mPedalHeldNotes.insert(kv);
        } else {
            mIsPedalDown = false;
            mPedalHeldNotes.clear();
        }
    }

    if (controller == 81) {
        if (mBassEnhanceEnabled) {
            mBassEnhanceRatio = value / 127.0f;
            mBassWeightsDirty = true;
        } else {
            mBassVolumeFromCC = value / 127.0f;  // 贝斯音量 (待 native-lib 读取)
        }
    } else if (controller == 18) {
        mBassEnhanceCenter = 36 + (int)(value / 127.0f * 24);
        mBassWeightsDirty = true;
    } else if (controller == 19) {
        mBassEnhanceSpread = 5 + (int)(value / 127.0f * 43);
        mBassWeightsDirty = true;
    }

    return r;
}

// ---- Auto-Sustain ----

int MidiProcessor::setAutoSustainEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(mLock);
    auto asa = mAutoSustain.setEnabled(enabled);
    if (asa == AutoSustainManager::CC64_ON) {
        mIsPedalDown = true;
        for (const auto& kv : mActiveNotes) mPedalHeldNotes.insert(kv);
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

int MidiProcessor::changeTranspose(int delta) {
    std::lock_guard<std::mutex> lock(mLock);
    mTranspose += delta;
    if (mTranspose < -12) mTranspose = -12;
    if (mTranspose > 12) mTranspose = 12;
    return mTranspose;
}

std::string MidiProcessor::getNoteStateJson() {
    std::lock_guard<std::mutex> lock(mLock);
    auto lowNotes = getLowNotes();
    auto allNotes = getAllNotes();

    auto setJson = [](const std::set<int>& s) -> std::string {
        std::string j; bool first = true;
        for (int n : s) { if (!first) j += ","; first = false; j += std::to_string(n); }
        return j;
    };
    auto mapJson = [](const std::map<int, NoteInfo>& m) -> std::string {
        std::string j; bool first = true;
        for (const auto& kv : m) {
            if (!first) j += ","; first = false;
            j += "{\"p\":" + std::to_string(kv.first) + ",\"v\":" + std::to_string(kv.second.velocity) + ",\"t\":" + std::to_string((int64_t)kv.second.timestampMs) + "}";
        }
        return j;
    };
    std::set<int> lowSet; for (const auto& kv : lowNotes) lowSet.insert(kv.first);

    std::string json = "{\"active\":[" + mapJson(mActiveNotes) +
               "],\"pedal\":[" + mapJson(mPedalHeldNotes) +
               "],\"low\":[" + setJson(lowSet) +
               "],\"all\":[" + setJson(allNotes) +
               "],\"pedalDown\":" + std::string(mIsPedalDown ? "true" : "false") +
               ",\"split\":" + std::to_string(mSplitPoint) +
               ",\"autoSustain\":" + std::string(mAutoSustain.isEnabled() ? "true" : "false") +
               ",\"isBreaking\":" + std::string(mAutoSustain.isBreaking() ? "true" : "false") +
               ",\"bassEnhance\":" + std::string(mBassEnhanceEnabled ? "true" : "false") +
               ",\"bassRatio\":" + std::to_string(mBassEnhanceRatio).substr(0, 4) +
               ",\"transpose\":" + std::to_string(mTranspose) +
               ",\"bassCenter\":" + std::to_string(mBassEnhanceCenter) +
               ",\"bassSpread\":" + std::to_string(mBassEnhanceSpread) +
               ",\"bassWeights\":[";
    if (mBassWeightsDirty) const_cast<MidiProcessor*>(this)->recomputeBassWeights();
    bool firstW = true;
    for (int i = 0; i < 128; i++) {
        if (!firstW) json += ","; firstW = false;
        char buf[16];
        snprintf(buf, sizeof(buf), "%.3f", mBassWeights[i]);
        json += buf;
    }
    json += "]}";
    return json;
}

std::string MidiProcessor::getChordInfo() {
    std::lock_guard<std::mutex> lock(mLock);
    auto allNotes = getAllNotes();
    std::vector<int> notes(allNotes.begin(), allNotes.end());
    auto chords = detect_chord(notes);
    mPrimaryChord = chords.empty() ? "--" : chords[0];
    mSecondaryChord = chords.size() > 1 ? chords[1] : "";
    int scaleRoot = mScaleDetector.getScaleRootPc();
    mKeyName = mScaleDetector.getKeyName();
    if (mPrimaryChord != "--" && scaleRoot >= 0) {
        int rootPc = -1;
        std::string rootStr;
        if (mPrimaryChord.size() >= 1) {
            for (int i = 0; i < 12; i++) {
                std::string pn(pitchNames[i]);
                if (mPrimaryChord.compare(0, pn.size(), pn) == 0) { rootPc = i; rootStr = pn; break; }
            }
            if (rootPc < 0 && mPrimaryChord.size() >= 2) {
                for (int i = 0; i < 12; i++) {
                    std::string pn(pitchNames[i]);
                    if (pn.size() == 2 && mPrimaryChord.compare(0, 2, pn) == 0) { rootPc = i; rootStr = pn; break; }
                }
            }
        }
        if (rootPc >= 0) {
            static const char* romanMajor[] = {"I","","ii","","iii","IV","","V","","vi","","viidim"};
            static const char* romanMinor[] = {"i","","iidim","III","","iv","","v","","VI","","viidim"};
            int degree = (rootPc - scaleRoot + 12) % 12;
            mTsdDegree = mScaleDetector.isMinor() ? romanMinor[degree] : romanMajor[degree];
            if (mTsdDegree.empty()) mTsdDegree = mPrimaryChord;
            std::string suffix = mPrimaryChord.substr(rootStr.size());
            size_t slash = suffix.find('/');
            if (slash != std::string::npos) suffix = suffix.substr(0, slash);
            mRomanNumeral = mTsdDegree + suffix;
        } else { mRomanNumeral = mPrimaryChord; mTsdDegree = mPrimaryChord; }
    } else { mRomanNumeral = "--"; mTsdDegree = "--"; }
    return mPrimaryChord + "|" + mRomanNumeral + "|" + mKeyName + "|" + mTsdDegree + "|" + mSecondaryChord;
}

std::string MidiProcessor::getRomanFromChord(const std::string& chordName, int rootPc) {
    std::lock_guard<std::mutex> lock(mLock);
    if (chordName.empty() || chordName == "-" || rootPc < 0)
        return "--|--|--";
    int scaleRoot = mScaleDetector.getScaleRootPc();
    std::string keyName = mScaleDetector.getKeyName();
    if (scaleRoot < 0) return chordName + "|--|" + keyName;
    int degree = (rootPc - scaleRoot + 12) % 12;
    static const char* romanMajor[] = {"I","","ii","","iii","IV","","V","","vi","","viidim"};
    static const char* romanMinor[] = {"i","","iidim","III","","iv","","v","","VI","","viidim"};
    const char* tsd = mScaleDetector.isMinor() ? romanMinor[degree] : romanMajor[degree];
    if (!tsd || tsd[0] == 0) tsd = chordName.c_str();
    // Extract suffix from chord name (remove root prefix)
    const char* nn[12] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
    std::string rootStr = nn[rootPc];
    std::string suffix = chordName.substr(rootStr.size());
    // Remove bass (slash) from suffix
    size_t slash = suffix.find('/');
    if (slash != std::string::npos) suffix = suffix.substr(0, slash);
    std::string roman = std::string(tsd) + suffix;
    return roman + "|" + keyName + "|" + tsd;
}
