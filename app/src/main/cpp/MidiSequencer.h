#pragma once
#include <vector>
#include <atomic>
#include <functional>

// 定义简单的 MIDI 事件
struct MidiEvent {
    double timestampSeconds; // 使用秒作为单位，更通用
    int type;    // 0x90 NoteOn, 0x80 NoteOff
    int channel;
    int note;
    int velocity;
};

// 前向声明，避免循环引用
class AudioEngine;

class MidiSequencer {
public:
    // 传入 AudioEngine 指针，以便回调
    MidiSequencer(AudioEngine* engine) : mEngine(engine) {}

    void setSequence(const std::vector<MidiEvent>& events, double loopDuration);
    void play();
    void stop();

    // 核心心跳函数：告诉 Sequencer 过去了多少时间
    void tick(double deltaTime);

private:
    AudioEngine* mEngine;
    std::vector<MidiEvent> mSequence;
    double mLoopDuration = 0.0;

    std::atomic<bool> mIsPlaying {false};
    int mPlayIndex = 0;
    double mCurrentTime = 0.0;
};