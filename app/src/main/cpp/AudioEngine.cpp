#include "AudioEngine.h"
#include "RhythmAudioEngine.h"
#include "DrumLoopEngine.h"

// Global rhythm engine for WAV playback
RhythmAudioEngine* g_rhythmEngine = nullptr;
// Global drum loop engine (opus → PCM 循环混音)
DrumLoopEngine* g_drumLoopEngine = nullptr;
extern float g_bassVolume;
#include "fluid_sfont.h"
#include <android/log.h>
#include <cstring>
#include <cstdio>
#include <cmath>

#define TAG "AudioEngine"

AudioEngine::~AudioEngine() { stop(); }

void AudioEngine::scanPresets(fluid_synth_t* synth, std::vector<InstrumentInfo>& list) {
    list.clear();
    if (!synth) return;
    fluid_sfont_t* sfont = fluid_synth_get_sfont(synth, 0);
    if (!sfont) return;
    int banksToCheck[] = {0, 128, 1, 2, 3, 4, 5, 6, 7, 8};
    for (int b = 0; b < 10; b++) {
        int bank = banksToCheck[b];
        for (int prog = 0; prog < 128; prog++) {
            fluid_preset_t* preset = fluid_sfont_get_preset(sfont, bank, prog);
            if (preset) {
                const char* name = fluid_preset_get_name(preset);
                if (!name) name = "Unknown";
                char displayName[256];
                if (bank == 128) snprintf(displayName, sizeof(displayName), "%s (Drum Kit)", name);
                else if (bank != 0) snprintf(displayName, sizeof(displayName), "%s (Bank %d)", name, bank);
                else snprintf(displayName, sizeof(displayName), "%s", name);
                list.push_back({std::string(displayName), bank, prog});
            }
        }
    }
}

bool AudioEngine::init(const char* sf2Path) {
    std::lock_guard<std::mutex> lock(mLock);
    if (mLeadSynth) { delete_fluid_synth(mLeadSynth); mLeadSynth = nullptr; }
    if (mAccompSynth) { delete_fluid_synth(mAccompSynth); mAccompSynth = nullptr; }
    if (mBassSynth) { delete_fluid_synth(mBassSynth); mBassSynth = nullptr; }
    if (mSettings) { delete_fluid_settings(mSettings); mSettings = nullptr; }
    mLeadInstruments.clear(); mAccompInstruments.clear();

    mSettings = new_fluid_settings();
    fluid_settings_setint(mSettings, "synth.polyphony", 128);
    mLeadSynth = new_fluid_synth(mSettings);
    mAccompSynth = new_fluid_synth(mSettings);
    mBassSynth = new_fluid_synth(mSettings);
    if (!mLeadSynth || !mAccompSynth || !mBassSynth) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "Failed to create synths");
        return false;
    }

    mLeadGain = 0.5;  // 10% of 5.0
    mAccompGain = 3.6;
    fluid_synth_set_gain(mLeadSynth, 0.5f); // 10% of 5.0
    fluid_synth_set_interp_method(mLeadSynth, -1, FLUID_INTERP_LINEAR);
    fluid_synth_set_reverb(mLeadSynth, 0.85, 0.15, 0.8, 0.70);
    mLeadSoundFontId = fluid_synth_sfload(mLeadSynth, sf2Path, 1);
    if (mLeadSoundFontId != -1) {
        scanPresets(mLeadSynth, mLeadInstruments);
        fluid_synth_program_change(mLeadSynth, 0, 89); // Warm Pad
        fluid_synth_cc(mLeadSynth, 0, 91, 30);
    }
    fluid_synth_set_gain(mAccompSynth, 3.6f);
    fluid_synth_set_interp_method(mAccompSynth, -1, FLUID_INTERP_LINEAR);
    fluid_synth_set_reverb(mAccompSynth, 0.85, 0.15, 0.8, 0.70);
    fluid_synth_set_gain(mBassSynth, 0.8f);
    fluid_synth_set_interp_method(mBassSynth, -1, FLUID_INTERP_LINEAR);
    fluid_synth_set_reverb(mBassSynth, 0.0, 0.0, 0.0, 0.0);  // 贝斯干声，制音时立即切断
    // 预加载默认 SF2 防止回调 crash, 后续 nativeLoadBassSoundFont 会覆盖
    mBassSoundFontId = fluid_synth_sfload(mBassSynth, sf2Path, 1);
    return true;
}

void AudioEngine::start() {
    if (stream) { stream->stop(); stream->close(); stream.reset(); }
    oboe::AudioStreamBuilder builder;
    builder.setDirection(oboe::Direction::Output)
        ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
        ->setSharingMode(oboe::SharingMode::Shared)
        ->setFormat(oboe::AudioFormat::Float)
        ->setChannelCount(oboe::ChannelCount::Stereo)
        ->setUsage(oboe::Usage::Game)
        ->setSampleRate(44100)
        ->setCallback(this);
    oboe::Result result = builder.openStream(stream);
    if (result != oboe::Result::OK) return;
    int32_t bufferSize = stream->getFramesPerBurst() * 4;
    stream->setBufferSizeInFrames(bufferSize);
    { std::lock_guard<std::mutex> lock(mLock); mMixBuffer.assign(bufferSize * 2, 0.0f); }
    stream->requestStart();
}

void AudioEngine::restartStream() {
    // 仅关闭和重开 Oboe 流, 不碰 FluidSynth
    if (stream) { stream->stop(); stream->close(); stream.reset(); }
    start();
}

void AudioEngine::stop() {
    if (stream) { stream->stop(); stream->close(); stream.reset(); }
    std::lock_guard<std::mutex> lock(mLock);
    if (mLeadSynth) { delete_fluid_synth(mLeadSynth); mLeadSynth = nullptr; }
    if (mAccompSynth) { delete_fluid_synth(mAccompSynth); mAccompSynth = nullptr; }
    if (mBassSynth) { delete_fluid_synth(mBassSynth); mBassSynth = nullptr; }
    if (mSettings) { delete_fluid_settings(mSettings); mSettings = nullptr; }
}

// ===== MIDI command queue (lock-free from callback perspective) =====

void AudioEngine::enqueueNoteOn(int target, int channel, int note, int velocity) {
    std::lock_guard<std::mutex> lock(mCmdMutex);
    mCmdQueue.push_back({MidiCmdType::NoteOn, target, channel, note, velocity});
}
void AudioEngine::enqueueNoteOff(int target, int channel, int note) {
    std::lock_guard<std::mutex> lock(mCmdMutex);
    mCmdQueue.push_back({MidiCmdType::NoteOff, target, channel, note, 0});
}
void AudioEngine::enqueueCC(int target, int channel, int controller, int value) {
    std::lock_guard<std::mutex> lock(mCmdMutex);
    mCmdQueue.push_back({MidiCmdType::CC, target, channel, controller, value});
}
void AudioEngine::enqueueProgramChange(int target, int channel, int bank, int program) {
    std::lock_guard<std::mutex> lock(mCmdMutex);
    mCmdQueue.push_back({MidiCmdType::ProgramChange, target, channel, bank, program});
}
void AudioEngine::enqueueAllNotesOff(int target, int channel) {
    std::lock_guard<std::mutex> lock(mCmdMutex);
    mCmdQueue.push_back({MidiCmdType::AllNotesOff, target, channel, 0, 0});
}
void AudioEngine::enqueueAllSoundsOff(int target) {
    std::lock_guard<std::mutex> lock(mCmdMutex);
    mCmdQueue.push_back({MidiCmdType::AllSoundsOff, target, 0, 0, 0});
}
void AudioEngine::enqueueSetReverb(int target, double roomSize, double level) {
    std::lock_guard<std::mutex> lock(mCmdMutex);
    mCmdQueue.push_back({MidiCmdType::SetReverb, target, 0, 0, 0, roomSize, level});
}

void AudioEngine::enqueueSetGain(int target, double gain) {
    if (target == 0) mLeadGain = gain;
    else if (target == 1) mAccompGain = gain;
    else mBassGain = gain;
    std::lock_guard<std::mutex> lock(mCmdMutex);
    mCmdQueue.push_back({MidiCmdType::SetGain, target, 0, 0, 0, gain, 0});
}

double AudioEngine::getGain(int target) {
    if (target == 0) return mLeadGain;
    else if (target == 1) return mAccompGain;
    else return mBassGain;
}

double AudioEngine::getReverbRoomSize(int target) {
    fluid_synth_t* synth = (target == 0) ? mLeadSynth : (target == 1) ? mAccompSynth : mBassSynth;
    return synth ? fluid_synth_get_reverb_roomsize(synth) : 0.0;
}

double AudioEngine::getReverbLevel(int target) {
    fluid_synth_t* synth = (target == 0) ? mLeadSynth : (target == 1) ? mAccompSynth : mBassSynth;
    return synth ? fluid_synth_get_reverb_level(synth) : 0.0;
}

void AudioEngine::execCommand(const MidiCmd& cmd) {
    fluid_synth_t* synth = (cmd.target == 0) ? mLeadSynth : (cmd.target == 1) ? mAccompSynth : mBassSynth;
    if (!synth) return;
    switch (cmd.type) {
        case MidiCmdType::NoteOn:
            fluid_synth_noteon(synth, cmd.channel, cmd.data1, cmd.data2);
            break;
        case MidiCmdType::NoteOff:
            fluid_synth_noteoff(synth, cmd.channel, cmd.data1);
            break;
        case MidiCmdType::CC:
            fluid_synth_cc(synth, cmd.channel, cmd.data1, cmd.data2);
            break;
        case MidiCmdType::ProgramChange:
            fluid_synth_bank_select(synth, cmd.channel, cmd.data1);
            fluid_synth_program_change(synth, cmd.channel, cmd.data2);
            break;
        case MidiCmdType::AllNotesOff:
            fluid_synth_all_notes_off(synth, cmd.channel);
            break;
        case MidiCmdType::AllSoundsOff:
            for (int ch = 0; ch < 16; ++ch) fluid_synth_all_sounds_off(synth, ch);
            break;
        case MidiCmdType::SetReverb:
            fluid_synth_set_reverb(synth, cmd.fdata1, 0.1, 0.8, cmd.fdata2);
            break;
        case MidiCmdType::SetGain:
            fluid_synth_set_gain(synth, (float)cmd.fdata1);
            if (cmd.target == 0) mLeadGain = cmd.fdata1;
            else if (cmd.target == 1) mAccompGain = cmd.fdata1;
            else mBassGain = cmd.fdata1;
            break;
    }
}

void AudioEngine::processPendingCommands() {
    // Swap queue under lock (microseconds), then process lock-free
    std::vector<MidiCmd> batch;
    {
        std::lock_guard<std::mutex> lock(mCmdMutex);
        if (mCmdQueue.empty()) return;
        batch.swap(mCmdQueue);
    }
    for (const auto& cmd : batch) execCommand(cmd);
}

// ===== Oboe callback (no lock) =====

oboe::DataCallbackResult AudioEngine::onAudioReady(
        oboe::AudioStream *audioStream, void *audioData, int32_t numFrames) {
    float *outBuffer = static_cast<float *>(audioData);
#if defined(__arm__)
    uint32_t fpscr;
    asm volatile("vmrs %0, fpscr" : "=r"(fpscr));
    asm volatile("vmsr fpscr, %0" : : "r" (fpscr | (1U << 24)));
#elif defined(__aarch64__)
    uint64_t fpscr;
    asm volatile("mrs %0, fpcr" : "=r"(fpscr));
    asm volatile("msr fpcr, %0" : : "r" (fpscr | (1U << 24)));
#endif
    // Process all pending MIDI commands (swap under µs lock, then render lock-free)
    processPendingCommands();

    if (mLeadSynth && mAccompSynth) {
        fluid_synth_write_float(mLeadSynth, numFrames, outBuffer, 0, 2, outBuffer, 1, 2);
        float* mixPtr = mMixBuffer.data();
        if (mMixBuffer.size() >= (size_t)numFrames * 2) {
            fluid_synth_write_float(mAccompSynth, numFrames, mixPtr, 0, 2, mixPtr, 1, 2);
            for (int i = 0; i < numFrames * 2; ++i)
                outBuffer[i] = tanhf(outBuffer[i] + mixPtr[i]);
        }
    } else {
        memset(outBuffer, 0, numFrames * 2 * sizeof(float));
    }
    // Bass synth → RhythmAudioEngine reverb → mix
    if (mBassSynth && g_rhythmEngine) {
        float* bassBuf = mMixBuffer.data();
        if (mMixBuffer.size() >= (size_t)numFrames * 2) {
            memset(bassBuf, 0, numFrames * 2 * sizeof(float));
            fluid_synth_write_float(mBassSynth, numFrames, bassBuf, 0, 2, bassBuf, 1, 2);
            g_rhythmEngine->processBassReverb(bassBuf, numFrames);
            float rg = g_rhythmEngine ? g_rhythmEngine->getMasterGain() : 0.0f;
            for (int i = 0; i < numFrames * 2; ++i)
                outBuffer[i] += bassBuf[i] * (float)mBassGain * 1.8f * rg / 4.0f * g_bassVolume;
        }
    }
    // Mix rhythm WAV samples, then soft-clip to prevent hard clipping
    if (g_rhythmEngine) {
        g_rhythmEngine->mixAudio(outBuffer, numFrames);
        for (int i = 0; i < numFrames * 2; ++i)
            outBuffer[i] = tanhf(outBuffer[i]);
    }
    // Mix drum loops (opus 循环), then soft-clip
    if (g_drumLoopEngine) {
        g_drumLoopEngine->mixAudio(outBuffer, numFrames, audioStream->getSampleRate());
        for (int i = 0; i < numFrames * 2; ++i)
            outBuffer[i] = tanhf(outBuffer[i]);
    }
    return oboe::DataCallbackResult::Continue;
}

// ===== SoundFont management (uses mLock, direct synth access) =====

bool AudioEngine::loadSoundFont(int target, const char* path) {
    std::lock_guard<std::mutex> lock(mLock);
    fluid_synth_t* synth = (target == 0) ? mLeadSynth : (target == 1) ? mAccompSynth : mBassSynth;
    int* sfId = (target == 0) ? &mLeadSoundFontId : (target == 1) ? &mAccompSoundFontId : &mBassSoundFontId;
    if (!synth) return false;
    if (*sfId != -1) { fluid_synth_sfunload(synth, *sfId, 1); *sfId = -1; }
    *sfId = fluid_synth_sfload(synth, path, 1);
    if (*sfId == -1) return false;
    if (target == 2) return true; // Bass synth: 不需要 scan preset 列表
    auto* list = (target == 0) ? &mLeadInstruments : &mAccompInstruments;
    scanPresets(synth, *list);
    if (!list->empty()) {
        fluid_synth_bank_select(synth, 0, (*list)[0].bank);
        fluid_synth_program_change(synth, 0, (*list)[0].program);
    }
    fluid_synth_cc(synth, 0, 91, 127);
    return true;
}

int AudioEngine::getInstrumentCount(int target) {
    std::lock_guard<std::mutex> lock(mLock);
    if (target == 0) return (int)mLeadInstruments.size();
    if (target == 1) return (int)mAccompInstruments.size();
    return 0;
}

const char* AudioEngine::getInstrumentName(int target, int index) {
    std::lock_guard<std::mutex> lock(mLock);
    auto* list = (target == 0) ? &mLeadInstruments : &mAccompInstruments;
    if (index < 0 || index >= (int)list->size()) return "Unknown";
    return (*list)[index].name.c_str();
}

void AudioEngine::setInstrument(int target, int index) {
    std::lock_guard<std::mutex> lock(mLock);
    fluid_synth_t* synth = (target == 0) ? mLeadSynth : mAccompSynth;
    auto* list = (target == 0) ? &mLeadInstruments : &mAccompInstruments;
    if (synth && index >= 0 && index < (int)list->size()) {
        fluid_synth_bank_select(synth, 0, (*list)[index].bank);
        fluid_synth_program_change(synth, 0, (*list)[index].program);
    }
}

int AudioEngine::getInstrumentBank(int target, int index) {
    std::lock_guard<std::mutex> lock(mLock);
    auto* list = (target == 0) ? &mLeadInstruments : &mAccompInstruments;
    if (index < 0 || index >= (int)list->size()) return 0;
    return (*list)[index].bank;
}

void AudioEngine::setMasterVolume(int target, float gain) {
    std::lock_guard<std::mutex> lock(mLock);
    if (target == 0 && mLeadSynth) fluid_synth_set_gain(mLeadSynth, gain);
    else if (target == 1 && mAccompSynth) fluid_synth_set_gain(mAccompSynth, gain);
}
