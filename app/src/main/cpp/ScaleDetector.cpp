#include "ScaleDetector.h"
#include <cmath>
#include <algorithm>

static const char* PITCH_NAMES[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

// Krumhansl-Kessler major key profile (tonal hierarchy for C major)
static const float KS_MAJOR[12] = {
    6.35f, 2.23f, 3.48f, 2.33f, 4.38f, 4.09f,
    2.52f, 5.19f, 2.39f, 3.66f, 2.29f, 2.88f
};

// Minor profile = major profile rotated so A (index 9) gets C's tonic weight
// KS_MINOR[i] = KS_MAJOR[(i + 3) % 12]
static float KS_MINOR[12];

static bool sInitialized = false;
static void initMinorProfile() {
    if (sInitialized) return;
    for (int i = 0; i < 12; i++) {
        KS_MINOR[i] = KS_MAJOR[(i + 3) % 12];
    }
    sInitialized = true;
}

void ScaleDetector::feedNote(int midiNote) {
    initMinorProfile();

    // Decay all weights slightly (old notes lose influence)
    for (int i = 0; i < 12; i++) {
        mWeights[i] *= 0.98f;
    }

    // Weight: lower notes count more for key detection (bass defines harmony)
    float w = 2.0f - (float)(midiNote - 24) / 96.0f * 1.9f;
    if (w < 0.1f) w = 0.1f;
    if (w > 2.0f) w = 2.0f;

    mWeights[midiNote % 12] += w;

    // Mark cache as dirty
    mRecalcCounter = 0;
}

void ScaleDetector::tick(float deltaSeconds) {
    // Decay all weights over time. Goal: ~2% decay per 500ms
    float decayPerSecond = 0.04f;  // 4% per second without new input
    float factor = 1.0f - decayPerSecond * deltaSeconds;
    if (factor < 0.0f) factor = 0.0f;

    float total = 0.0f;
    for (int i = 0; i < 12; i++) {
        mWeights[i] *= factor;
        total += mWeights[i];
    }

    if (total < 0.05f) {
        mWeights.fill(0.0f);
    }
}

void ScaleDetector::recalcScale() const {
    float totalWeight = 0.0f;
    for (int i = 0; i < 12; i++) totalWeight += mWeights[i];

    if (totalWeight < 0.05f) {
        mCachedRootPc = -1;
        mCachedIsMinor = false;
        mCachedTotalWeight = totalWeight;
        return;
    }

    float bestScore = -1e9f;
    int bestRoot = 0;
    bool bestIsMinor = false;

    for (int tonic = 0; tonic < 12; tonic++) {
        // Major score: dot(weights, major_template_rotated_to_tonic)
        float majorScore = 0.0f;
        for (int i = 0; i < 12; i++) {
            majorScore += mWeights[i] * KS_MAJOR[(i - tonic + 12) % 12];
        }

        // Minor score: dot(weights, minor_template_rotated_to_tonic)
        float minorScore = 0.0f;
        for (int i = 0; i < 12; i++) {
            minorScore += mWeights[i] * KS_MINOR[(i - tonic + 12) % 12];
        }

        if (majorScore > bestScore) {
            bestScore = majorScore;
            bestRoot = tonic;
            bestIsMinor = false;
        }
        if (minorScore > bestScore) {
            bestScore = minorScore;
            bestRoot = tonic;
            bestIsMinor = true;
        }
    }

    mCachedRootPc = bestRoot;
    mCachedIsMinor = bestIsMinor;
    mCachedTotalWeight = totalWeight;
    mRecalcCounter = 20; // recalc every ~20 calls
}

int ScaleDetector::getScaleRootPc() const {
    if (mRecalcCounter <= 0) recalcScale();
    return mCachedRootPc;
}

std::string ScaleDetector::getKeyName() const {
    int root = getScaleRootPc();
    if (root < 0) return "--";

    std::string name = PITCH_NAMES[root];
    name += mCachedIsMinor ? "m小调" : "大调";
    return name;
}
