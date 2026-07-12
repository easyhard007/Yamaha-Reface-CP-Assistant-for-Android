#include "AudioEngine.h"
#include "fluid_sfont.h"
#include <android/log.h>
#include <cstring>
#include <cstdio>

#define TAG "AudioEngine"

// 析构函数
AudioEngine::~AudioEngine() {
    stop();
}

// 辅助：扫描乐器 (逻辑不变，只是参数化了)
void AudioEngine::scanPresets(fluid_synth_t* synth, std::vector<InstrumentInfo>& list) {
    list.clear();
    if (!synth) return;

    fluid_sfont_t* sfont = fluid_synth_get_sfont(synth, 0);
    if (!sfont) return;

    // 智能扫描 Bank 0-8 和 128(鼓)
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

    mSettings = new_fluid_settings();
    fluid_settings_setnum(mSettings, "synth.sample-rate", 48000);
    fluid_settings_setint(mSettings, "synth.polyphony", 64);
    fluid_settings_setstr(mSettings, "synth.interpolation-method", "4th order sinc");

    // >>>>> 创建两个 Synth >>>>>
    mLeadSynth = new_fluid_synth(mSettings);
    mAccompSynth = new_fluid_synth(mSettings);

    if (!mLeadSynth || !mAccompSynth) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "Failed to create synths");
        return false;
    }

    // 初始化 Lead Synth (加载默认音色)
    fluid_synth_set_gain(mLeadSynth, 1.0f);
    mLeadSoundFontId = fluid_synth_sfload(mLeadSynth, sf2Path, 1);
    if (mLeadSoundFontId != -1) {
        scanPresets(mLeadSynth, mLeadInstruments);
        fluid_synth_set_reverb_on(mLeadSynth, 1);
        fluid_synth_set_reverb(mLeadSynth, 0.8f, 0.8f, 50.0f, 0.2f);
        fluid_synth_cc(mLeadSynth, 0, 91, 127);
    }

    // 初始化 Accomp Synth (先不加载音色，或者加载同一个，这里先留空待用)
    // 为了防止空指针，设置一下默认增益
    fluid_synth_set_gain(mAccompSynth, 0.8f); // 伴奏音量稍微小一点点
    fluid_synth_set_reverb_on(mAccompSynth, 1);
    fluid_synth_set_reverb(mAccompSynth, 0.8f, 0.8f, 50.0f, 0.2f); // 给伴奏也加混响

    return true;
}

void AudioEngine::start() {
    oboe::AudioStreamBuilder builder;
    builder.setDirection(oboe::Direction::Output)
            ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
            ->setSharingMode(oboe::SharingMode::Shared)
            ->setFormat(oboe::AudioFormat::Float)
            ->setChannelCount(oboe::ChannelCount::Stereo)
            ->setSampleRate(48000)
            ->setCallback(this);

    oboe::Result result = builder.openStream(stream);
    if (result != oboe::Result::OK) return;

    int32_t burstFrames = stream->getFramesPerBurst();
    stream->setBufferSizeInFrames(burstFrames * 4);
    stream->requestStart();
}

void AudioEngine::stop() {
    if (stream) {
        stream->stop();
        stream->close();
        stream.reset();
    }

    std::lock_guard<std::mutex> lock(mLock);

    // >>>>> 释放两个 Synth >>>>>
    if (mLeadSynth) { delete_fluid_synth(mLeadSynth); mLeadSynth = nullptr; }
    if (mAccompSynth) { delete_fluid_synth(mAccompSynth); mAccompSynth = nullptr; }
    if (mSettings) { delete_fluid_settings(mSettings); mSettings = nullptr; }
}

// =============================================================
// 核心逻辑：根据 target 参数分发
// target: 0 = Lead, 1 = Accomp
// =============================================================

void AudioEngine::playNote(int target, int note, int velocity) {
    std::lock_guard<std::mutex> lock(mLock);
    if (target == 0 && mLeadSynth) {
        fluid_synth_noteon(mLeadSynth, 0, note, velocity);
    } else if (target == 1 && mAccompSynth) {
        fluid_synth_noteon(mAccompSynth, 0, note, velocity);
    }
}

void AudioEngine::stopNote(int target, int note) {
    std::lock_guard<std::mutex> lock(mLock);
    if (target == 0 && mLeadSynth) {
        fluid_synth_noteoff(mLeadSynth, 0, note);
    } else if (target == 1 && mAccompSynth) {
        fluid_synth_noteoff(mAccompSynth, 0, note);
    }
}

void AudioEngine::sendMidiControlChange(int target, int controller, int value) {
    std::lock_guard<std::mutex> lock(mLock);
    if (target == 0 && mLeadSynth) {
        fluid_synth_cc(mLeadSynth, 0, controller, value);
    } else if (target == 1 && mAccompSynth) {
        fluid_synth_cc(mAccompSynth, 0, controller, value);
    }
}

bool AudioEngine::loadSoundFont(int target, const char* path) {
    std::lock_guard<std::mutex> lock(mLock);

    fluid_synth_t* synth = (target == 0) ? mLeadSynth : mAccompSynth;
    int* sfId = (target == 0) ? &mLeadSoundFontId : &mAccompSoundFontId;
    auto* list = (target == 0) ? &mLeadInstruments : &mAccompInstruments;

    if (!synth) return false;

    if (*sfId != -1) {
        fluid_synth_sfunload(synth, *sfId, 1);
        *sfId = -1;
    }

    *sfId = fluid_synth_sfload(synth, path, 1);
    if (*sfId == -1) return false;

    // 扫描乐器
    scanPresets(synth, *list);

    // 默认选第一个
    if (!list->empty()) {
        fluid_synth_bank_select(synth, 0, (*list)[0].bank);
        fluid_synth_program_change(synth, 0, (*list)[0].program);
    }

    fluid_synth_cc(synth, 0, 91, 127); // 混响发送
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
    const std::vector<InstrumentInfo>* list = (target == 0) ? &mLeadInstruments : &mAccompInstruments;

    if (index < 0 || index >= list->size()) return "Unknown";
    return (*list)[index].name.c_str();
}

void AudioEngine::setInstrument(int target, int index) {
    std::lock_guard<std::mutex> lock(mLock);
    fluid_synth_t* synth = (target == 0) ? mLeadSynth : mAccompSynth;
    const std::vector<InstrumentInfo>* list = (target == 0) ? &mLeadInstruments : &mAccompInstruments;

    if (synth && index >= 0 && index < list->size()) {
        fluid_synth_bank_select(synth, 0, (*list)[index].bank);
        fluid_synth_program_change(synth, 0, (*list)[index].program);
    }
}

void AudioEngine::setMasterVolume(int target, float gain) {
    std::lock_guard<std::mutex> lock(mLock);
    if (target == 0 && mLeadSynth) fluid_synth_set_gain(mLeadSynth, gain);
    else if (target == 1 && mAccompSynth) fluid_synth_set_gain(mAccompSynth, gain);
}

// >>>>> 混音逻辑 >>>>>
oboe::DataCallbackResult AudioEngine::onAudioReady(
        oboe::AudioStream *audioStream,
        void *audioData,
        int32_t numFrames) {

    float *outBuffer = static_cast<float *>(audioData);

    // 确保混音缓冲区足够大
    if (mMixBuffer.size() < numFrames * 2) {
        mMixBuffer.resize(numFrames * 2);
    }

    std::unique_lock<std::mutex> lock(mLock, std::try_to_lock);
    if (lock.owns_lock() && mLeadSynth && mAccompSynth) {

        // 1. 渲染 Lead Synth 到输出缓冲区 (作为基底)
        fluid_synth_write_float(mLeadSynth, numFrames, outBuffer, 0, 2, outBuffer, 1, 2);

        // 2. 渲染 Accomp Synth 到临时缓冲区
        float* mixPtr = mMixBuffer.data();
        fluid_synth_write_float(mAccompSynth, numFrames, mixPtr, 0, 2, mixPtr, 1, 2);

        // 3. 混合 (Lead + Accomp)
        for (int i = 0; i < numFrames * 2; ++i) {
            outBuffer[i] += mixPtr[i]; // 简单的加法混合

            // 简单的软限幅 (Soft Limiter) 防止爆音
            // 如果不想有削波失真，可以乘以 0.7，但那样音量会变小
            if (outBuffer[i] > 1.0f) outBuffer[i] = 1.0f;
            else if (outBuffer[i] < -1.0f) outBuffer[i] = -1.0f;
        }

    } else {
        memset(outBuffer, 0, numFrames * 2 * sizeof(float));
    }

    return oboe::DataCallbackResult::Continue;
}