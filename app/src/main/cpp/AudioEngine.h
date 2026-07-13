#pragma once

#include <oboe/Oboe.h>
#include <mutex>
#include <memory>
#include <vector>
#include <string>
#include "fluidlite.h"

/// Audio-only engine — plays SoundFont via FluidLite + Oboe.
/// No MIDI logic, no note tracking, no auto-sustain.
class AudioEngine : public oboe::AudioStreamCallback {
public:
    AudioEngine() = default;
    ~AudioEngine();

    bool init(const char* sf2Path);
    void start();
    void stop();

    // Playback
    void playNote(int target, int note, int velocity);
    void stopNote(int target, int note);
    void sendCC(int target, int controller, int value); // FluidLite CC

    // SoundFont management
    bool loadSoundFont(int target, const char* path);
    int  getInstrumentCount(int target);
    const char* getInstrumentName(int target, int index);
    void setInstrument(int target, int index);
    void setMasterVolume(int target, float gain);

    // Oboe callback
    oboe::DataCallbackResult onAudioReady(
        oboe::AudioStream *audioStream, void *audioData, int32_t numFrames) override;

private:
    struct InstrumentInfo { std::string name; int bank; int program; };
    void scanPresets(fluid_synth_t* synth, std::vector<InstrumentInfo>& list);

    std::shared_ptr<oboe::AudioStream> stream;
    std::mutex mLock;

    fluid_settings_t* mSettings = nullptr;
    fluid_synth_t* mLeadSynth = nullptr;
    fluid_synth_t* mAccompSynth = nullptr;

    int mLeadSoundFontId = -1;
    int mAccompSoundFontId = -1;

    std::vector<InstrumentInfo> mLeadInstruments;
    std::vector<InstrumentInfo> mAccompInstruments;
    std::vector<float> mMixBuffer;
};
