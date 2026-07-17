#pragma once
#include <random>
#include <cmath>

class Humanizer {
public:
    Humanizer();

    /// 人性化一个音符, 返回 {velocityOffset, tickOffset}
    struct Result { int velOffset; float tickOffset; };
    Result humanize();

    float timingRandomness = 0.6f;
    float velocityRandomness = 0.6f;

private:
    std::mt19937 rng;
    std::normal_distribution<float> gauss;

    /// 生成限制在 [-limit, limit] 的随机值
    float randGauss(float limit);
};
