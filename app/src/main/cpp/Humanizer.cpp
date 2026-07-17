#include "Humanizer.h"
#include <algorithm>

Humanizer::Humanizer() : rng(std::random_device{}()), gauss(0.0f, 1.0f) {}

Humanizer::Result Humanizer::humanize() {
    static const float Z90 = 1.645f; // 90% 置信区间

    // 力度: 90% in ±10% of 127 = ±12.7 * randomness
    float velSigma = (12.7f * velocityRandomness) / Z90;
    float velRaw = randGauss(velSigma);
    int velOffset = (int)std::round(velRaw);

    // 时间: 90% in ±10ms = ±0.02 beat * randomness
    float beatSigma = (0.02f * timingRandomness) / Z90;
    float beatOffset = randGauss(beatSigma);

    return {velOffset, beatOffset};
}

float Humanizer::randGauss(float sigma) {
    if (sigma <= 0.0f) return 0.0f;
    return gauss(rng) * sigma;
}
