#pragma once

#include <oboe/Oboe.h>
#include <mutex>
#include <memory>
#include <vector>
#include <string>
#include "fluidlite.h"

enum class MidiCmdType { NoteOn, NoteOff, CC, ProgramChange, AllNotesOff, AllSoundsOff, SetReverb, SetGain };

struct MidiCmd {
    MidiCmdType type;
    int target;   // 0=lead, 1=accomp
    int channel;
    int data1;    // note / controller / bank
    int data2;    // velocity / value / program
    double fdata1; // reverb roomSize
    double fdata2; // reverb level
};

/// Audio-only engine — plays SoundFont via FluidLite + Oboe.
class AudioEngine : public oboe::AudioStreamCallback {
public:
    AudioEngine() = default;
    ~AudioEngine();

    bool init(const char* sf2Path);
    void start();
    void stop();

    // Enqueue MIDI commands (lock-free from callback perspective)
    void enqueueNoteOn(int target, int channel, int note, int velocity);
    void enqueueNoteOff(int target, int channel, int note);
    void enqueueCC(int target, int channel, int controller, int value);
    void enqueueProgramChange(int target, int channel, int bank, int program);
    void enqueueAllNotesOff(int target, int channel);
    void enqueueAllSoundsOff(int target);
    void enqueueSetReverb(int target, double roomSize, double level);
    void enqueueSetGain(int target, double gain);
    double getGain(int target);
    double getReverbRoomSize(int target);
    double getReverbLevel(int target);

    // Direct synth access (for init/load only — not thread-safe during playback)
    bool loadSoundFont(int target, const char* path);
    int  getInstrumentCount(int target);
    const char* getInstrumentName(int target, int index);
    void setInstrument(int target, int index);
    int  getInstrumentBank(int target, int index);
    void setMasterVolume(int target, float gain);

    // Oboe callback
    oboe::DataCallbackResult onAudioReady(
        oboe::AudioStream *audioStream, void *audioData, int32_t numFrames) override;

private:
    struct InstrumentInfo { std::string name; int bank; int program; };
    void scanPresets(fluid_synth_t* synth, std::vector<InstrumentInfo>& list);

    void processPendingCommands();
    void execCommand(const MidiCmd& cmd);

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

    // Lock-free MIDI command queue (swap-based)
    std::mutex mCmdMutex;
    std::vector<MidiCmd> mCmdQueue;
    double mLeadGain = 0.8;
    double mAccompGain = 0.8;
};
