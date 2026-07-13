#include "BassEnhancer.h"
#include <cmath>

BassNote BassEnhancer::processNoteOn(int note, int velocity, const float weights[128]) const {
    BassNote result = {0, 0, false};
    if (!mEnabled) return result;

    float w = weights[note];
    if (w <= 0.0f) return result;

    int targetVel = (int)std::round(velocity * w);
    if (targetVel <= 0) targetVel = 1;
    if (targetVel > 127) targetVel = 127;

    result.note = note - 12;
    result.velocity = targetVel;
    result.noteOn = true;
    return result;
}

BassNote BassEnhancer::processNoteOff(int note) const {
    BassNote result = {0, 0, false};
    if (!mEnabled) return result;
    result.note = note - 12;
    result.velocity = 0;
    result.noteOn = false;
    return result;
}
