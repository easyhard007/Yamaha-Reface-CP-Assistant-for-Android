#pragma once
#include <oboe/Oboe.h>
#include <mutex>
#include "SimpleReverb.h"

struct tsf;

class AudioEngine : public oboe::AudioStreamCallback {
public:
    void start();
    void stop();
    bool init(const char* sf2Path);
    void playNote(int note, int velocity);
    void stopNote(int note);
    void sendMidiControlChange(int controller, int value);

    int getInstrumentCount();
    const char* getInstrumentName(int index);
    void setInstrument(int index);

    oboe::DataCallbackResult onAudioReady(
            oboe::AudioStream *audioStream,
            void *audioData,
            int32_t numFrames) override;

private:
    std::shared_ptr<oboe::AudioStream> stream;
    tsf* g_TinySoundFont = nullptr;
    std::mutex mLock; // 锁
    SimpleReverb mReverb; //声明混响实例
};