#include "MidiSequencer.h"
#include "AudioEngine.h" // 需要引用 AudioEngine 来调用发声函数

void MidiSequencer::setSequence(const std::vector<MidiEvent>& events, double loopDuration) {
    mSequence = events;
    mLoopDuration = loopDuration;
    mPlayIndex = 0;
    mCurrentTime = 0.0;
}

void MidiSequencer::play() {
    mIsPlaying = true;
}

void MidiSequencer::stop() {
    mIsPlaying = false;
    mPlayIndex = 0;
    mCurrentTime = 0.0;
    // 这里以后可能需要调用 engine->allNotesOff() 来防止长音不断
}

// 这个函数将在 AudioEngine 的 onAudioReady 里被调用
void MidiSequencer::tick(double deltaTime) {
    if (!mIsPlaying || mSequence.empty()) return;

    double nextTime = mCurrentTime + deltaTime;

    // 遍历检查这段时间内是否有事件触发
    while (mPlayIndex < mSequence.size()) {
        const MidiEvent& evt = mSequence[mPlayIndex];

        if (evt.timestampSeconds <= nextTime) {
            // >>>>> 触发伴奏音符 >>>>>
            if (evt.type == 0x90) {
                // 这里就是你要的：调用 AudioEngine 的伴奏专用接口
                // TODO: 将来在这里加入和弦修改算法 (Chord Remapping)
                mEngine->playAccompNote(evt.channel, evt.note, evt.velocity);
            } else if (evt.type == 0x80) {
                mEngine->stopAccompNote(evt.channel, evt.note);
            }
            // <<<<<<<<<<<<<<<<<<<<<<<<<

            mPlayIndex++;
        } else {
            break; // 后面的事件还没到时间
        }
    }

    mCurrentTime = nextTime;

    // 简单的循环逻辑
    if (mLoopDuration > 0 && mCurrentTime >= mLoopDuration) {
        mCurrentTime -= mLoopDuration;
        mPlayIndex = 0;
    }
}