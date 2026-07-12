#include "AudioEngine.h"
#include "fluid_sfont.h"
#include <android/log.h>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <arm_neon.h>

#define TAG "AudioEngine"

AudioEngine::~AudioEngine() {
    stop();
}

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

    // 如果已经初始化过，先清理旧的（Activity 重建场景）
    if (mLeadSynth) { delete_fluid_synth(mLeadSynth); mLeadSynth = nullptr; }
    if (mAccompSynth) { delete_fluid_synth(mAccompSynth); mAccompSynth = nullptr; }
    if (mSettings) { delete_fluid_settings(mSettings); mSettings = nullptr; }
    mLeadInstruments.clear();
    mAccompInstruments.clear();

    mSettings = new_fluid_settings();
    fluid_settings_setint(mSettings, "synth.polyphony", 48);

    mLeadSynth = new_fluid_synth(mSettings);
    mAccompSynth = new_fluid_synth(mSettings);

    if (!mLeadSynth || !mAccompSynth) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "Failed to create synths");
        return false;
    }

    // 降低增益，为双合成器混音预留余量
    fluid_synth_set_gain(mLeadSynth, 0.6f);
    fluid_synth_set_interp_method(mLeadSynth, -1, FLUID_INTERP_LINEAR);
    mLeadSoundFontId = fluid_synth_sfload(mLeadSynth, sf2Path, 1);
    if (mLeadSoundFontId != -1) {
        scanPresets(mLeadSynth, mLeadInstruments);
//        fluid_synth_set_reverb_on(mLeadSynth, 1);
//        fluid_synth_set_reverb(mLeadSynth, 0.5f, 0.5f, 50.0f, 0.2f);
        fluid_synth_cc(mLeadSynth, 0, 91, 64);
    }

    fluid_synth_set_gain(mAccompSynth, 0.4f);
    fluid_synth_set_interp_method(mAccompSynth, -1, FLUID_INTERP_LINEAR);
//    fluid_synth_set_reverb_on(mAccompSynth, 1);
//    fluid_synth_set_reverb(mAccompSynth, 0.5f, 0.5f, 50.0f, 0.2f);

    return true;
}

void AudioEngine::start() {
    // 如果已有正在运行的 stream，先停掉旧的，防止两个 stream 同时跑导致崩溃
    if (stream) {
        stream->stop();
        stream->close();
        stream.reset();
    }

    oboe::AudioStreamBuilder builder;
    builder.setDirection(oboe::Direction::Output)
            ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
            ->setSharingMode(oboe::SharingMode::Exclusive) // 尝试独占模式
            ->setFormat(oboe::AudioFormat::Float)
            ->setChannelCount(oboe::ChannelCount::Stereo)
            ->setUsage(oboe::Usage::Game)
            ->setSampleRate(44100)  // 与 FluidLite 默认采样率保持一致，避免 resampling
            ->setCallback(this);

    oboe::Result result = builder.openStream(stream);
    if (result != oboe::Result::OK) return;

    // >>>>> 调优缓存大小 >>>>>
    // getFramesPerBurst 通常是设备最小的单位（如 128 或 192 帧）
    // 原来是 * 2，现在调大到 * 4，给 CPU 更多呼吸空间
    int32_t bufferSize = stream->getFramesPerBurst() * 4;
    stream->setBufferSizeInFrames(bufferSize);

    {
        std::lock_guard<std::mutex> lock(mLock);
        // 预分配混音缓冲区，确保足够容纳调大后的 bufferSize
        mMixBuffer.assign(bufferSize * 2, 0.0f);
    }

    stream->requestStart();
}

void AudioEngine::stop() {
    if (stream) {
        stream->stop();
        stream->close();
        stream.reset();
    }
    std::lock_guard<std::mutex> lock(mLock);
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
    fluid_synth_t* synth = (target == 0) ? mLeadSynth : mAccompSynth;
    if (synth) fluid_synth_noteon(synth, 0, note, velocity);
}

void AudioEngine::stopNote(int target, int note) {
    std::lock_guard<std::mutex> lock(mLock);
    fluid_synth_t* synth = (target == 0) ? mLeadSynth : mAccompSynth;
    if (synth) fluid_synth_noteoff(synth, 0, note);
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

    // [关键改进 1] 开启 FTZ (Flush-to-Zero) 模式，防止极小浮点数导致的 CPU 飙升
    // 仅针对 ARM 架构，如果是 x86 模拟器可跳过
#if defined(__arm__) || defined(__aarch64__)
    uintptr_t fpscr;
    asm volatile("mrs %0, fpcr" : "=r"(fpscr));
    asm volatile("msr fpcr, %0" : : "r" (fpscr | (1U << 24)));
#endif

    // 使用普通锁而非 try_to_lock：FluidLite 不是线程安全的，
    // write_float 与 noteon 并发会导致 SIGSEGV。
    // MIDI 操作（playNote 等）都是微秒级，短暂阻塞 audio 线程不会产生爆音。
    std::lock_guard<std::mutex> lock(mLock);

    if (mLeadSynth && mAccompSynth) {
        // 1. 渲染 Lead
        fluid_synth_write_float(mLeadSynth, numFrames, outBuffer, 0, 2, outBuffer, 1, 2);

        // 2. 渲染 Accomp 到预分配的临时缓冲区
        float* mixPtr = mMixBuffer.data();
        if (mMixBuffer.size() >= numFrames * 2) {
            fluid_synth_write_float(mAccompSynth, numFrames, mixPtr, 0, 2, mixPtr, 1, 2);

            // 3. 混合并使用 tanh 软削波 (取代硬削波，避免谐波失真)
            for (int i = 0; i < numFrames * 2; ++i) {
                float sample = outBuffer[i] + mixPtr[i];
                // tanh 软削波：在 ±1.0 附近平滑过渡，产生温暖的模拟式饱和而非刺耳的方波失真
                // 同时保留瞬态细节，不会像硬削波那样把峰值直接"切平"
                outBuffer[i] = tanhf(sample);
            }
        }
    } else {
        memset(outBuffer, 0, numFrames * 2 * sizeof(float));
    }

    return oboe::DataCallbackResult::Continue;
}