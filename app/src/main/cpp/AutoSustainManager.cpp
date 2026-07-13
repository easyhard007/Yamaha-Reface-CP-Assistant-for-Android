#include "AutoSustainManager.h"
#include <android/log.h>

#define TAG "AutoSustain"

AutoSustainManager::Action AutoSustainManager::setEnabled(bool enabled) {
    mEnabled = enabled;
    if (enabled) {
        __android_log_print(ANDROID_LOG_INFO, TAG, "AutoSustain ON");
        return CC64_ON;
    } else {
        mIsBreaking = false;
        __android_log_print(ANDROID_LOG_INFO, TAG, "AutoSustain OFF");
        return CC64_OFF;
    }
}

AutoSustainManager::Action AutoSustainManager::onNoteOn(int note, const std::set<int>& lowNotes) {
    if (!mEnabled) return NONE;

    // Collision detection
    bool collision = checkCollision(note, lowNotes);
    if (collision) {
        __android_log_print(ANDROID_LOG_INFO, TAG, "Collision detected, BREAK sustain");
        mIsBreaking = true;
        return CC64_OFF; // break: release pedal
    }

    return NONE;
}

AutoSustainManager::Action AutoSustainManager::onNoteOff(int note) {
    if (!mEnabled) return NONE;

    // try_repress: if in breaking state, any note-off triggers re-engage
    if (mIsBreaking) {
        mIsBreaking = false;
        __android_log_print(ANDROID_LOG_INFO, TAG, "REHOLD sustain");
        return REHOLD; // re-engage pedal + re-hold active notes
    }

    return NONE;
}

void AutoSustainManager::reset() {
    mIsBreaking = false;
}

bool AutoSustainManager::checkCollision(int newNote, const std::set<int>& lowNotes) const {
    if (lowNotes.empty()) return false;

    // Same note: allowed if it's the lowest or second-lowest
    if (lowNotes.count(newNote)) {
        auto it = lowNotes.begin();
        int lowest = *it;
        int secondLowest = (lowNotes.size() >= 2) ? *(++it) : lowest;
        if (newNote == lowest || newNote == secondLowest) return false;
    }

    // Rule 1: new note below existing lowest → collision
    if (newNote < *lowNotes.begin()) return true;

    // Rule 2: adjacent notes less than 3 semitones (major 2nd) → collision
    std::vector<int> notes(lowNotes.begin(), lowNotes.end());
    notes.push_back(newNote);
    std::sort(notes.begin(), notes.end());
    for (size_t i = 0; i < notes.size() - 1; i++) {
        if (notes[i + 1] - notes[i] < 3) return true;
    }
    return false;
}
