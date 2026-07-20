#pragma once
#include <atomic>

class BassAssist {
public:
    BassAssist() = default;

    void setEnabled(bool e) { mEnabled.store(e); }
    bool isEnabled() const { return mEnabled.load(); }

    void setVolume(float v) { mVolume.store(v); }
    float getVolume() const { return mVolume.load(); }

private:
    std::atomic<bool> mEnabled{true};  // 默认打开
    std::atomic<float> mVolume{0.8f};  // 默认 80%
};
