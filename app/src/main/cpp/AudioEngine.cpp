#include "AudioEngine.h"
#include "fluidlite/src/fluid_sfont.h"
#include <android/log.h>
#include <cstring> // for memset
#include <cstdio> // 用于 sprintf/snprintf


#define TAG "AudioEngine"

AudioEngine::~AudioEngine() {
    stop(); // 析构时确保资源释放
}

bool AudioEngine::init(const char* sf2Path) {
    std::lock_guard<std::mutex> lock(mLock);

    // 1. 创建 FluidSettings
    mSettings = new_fluid_settings();
    if (!mSettings) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "Failed to create fluid settings");
        return false;
    }

    // 2. 配置参数
    fluid_settings_setnum(mSettings, "synth.sample-rate", 48000);
    // 限制复音数，防止手机 CPU 过载爆音 (64 通常足够)
    fluid_settings_setint(mSettings, "synth.polyphony", 64);
//    // 关键：设置高音质插值 (7阶 Sinc)，这是 FluidLite 最大的优势
    fluid_settings_setstr(mSettings, "synth.interpolation-method", "linear");


    // 3. 创建合成器实例
    mSynth = new_fluid_synth(mSettings);
    if (!mSynth) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "Failed to create fluid synth");
        return false;
    }
    //设置音量
    fluid_synth_set_gain(mSynth, 1.6f);
    fluid_synth_cc(mSynth, 0, 91, 63);

    // 4. 加载 SF2 音色库
    // fluid_synth_sfload 返回一个 ID，-1 表示失败
    mSoundFontId = fluid_synth_sfload(mSynth, sf2Path, 1);

    if (mSoundFontId == -1) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "Failed to load SoundFont: %s", sf2Path);
        return false;
    }

    scanPresets();// 扫描乐器

    // 5. 启用内置效果器 (Reverb)
    fluid_synth_set_chorus_on(mSynth, 0); //关闭chorus效果
    fluid_synth_set_reverb_on(mSynth, 0);
    fluid_synth_set_reverb(mSynth, .9f, 0.95f, 20.0f, 0.1f);

//    // 打印日志验证版本
//    // 如果能在 Logcat 看到版本号，说明 FluidLite 库链接成功
//    __android_log_print(ANDROID_LOG_INFO, TAG, "Engine Verified: FluidLite %s", "1.2.2");
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
    if (result != oboe::Result::OK) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "Failed to open Oboe stream: %s", oboe::convertToText(result));
        return;
    }

    // >>>>> 关键修复：手动调整缓冲区大小 (The Sweet Spot) >>>>>

    // 获取系统的最佳 Burst 大小 (通常是 192 帧)
    int32_t burstFrames = stream->getFramesPerBurst();

    // 打印一下，让你心里有数 (在 Logcat 里看)
    __android_log_print(ANDROID_LOG_INFO, TAG, "Hardware Burst Size: %d", burstFrames);

    // 设置缓冲区倍数 (Multiples)
    // 倍数 1: 极限延迟 (极易爆音)
    // 倍数 2: 标准延迟 (轻量级乐器OK，大钢琴可能偶尔爆音)
    // 倍数 4: 安全延迟 (推荐！大钢琴也能稳住，延迟增加约 10ms，完全可接受)

    // 我们尝试设置为 4 倍 Burst
    int32_t targetFrames = burstFrames * 4;

    // setBufferSizeInFrames 会返回实际设置成功的大小
    auto setSize = stream->setBufferSizeInFrames(targetFrames);

    __android_log_print(ANDROID_LOG_INFO, TAG, "Buffer resized to: %d frames (Target: %d)", setSize.value(), targetFrames);
    // <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<

    // 启动音频流
    stream->requestStart();
}

void AudioEngine::stop() {
    // 停止 Oboe 流
    if (stream) {
        stream->stop();
        stream->close();
        stream.reset();
    }

    std::lock_guard<std::mutex> lock(mLock);
    // 释放 FluidLite 资源
    if (mSynth) {
        delete_fluid_synth(mSynth);
        mSynth = nullptr;
    }
    if (mSettings) {
        delete_fluid_settings(mSettings);
        mSettings = nullptr;
    }
}

// =============================================================
// MIDI 处理 (线程安全)
// =============================================================

void AudioEngine::playNote(int note, int velocity) {
    // 虽然 FluidLite 内部有锁，但为了防止在 stop() 销毁过程中调用，我们在外层也加锁
    std::lock_guard<std::mutex> lock(mLock);
    if (mSynth) {
        // Channel 0, Note, Velocity
        fluid_synth_noteon(mSynth, 0, note, velocity);
    }
}

void AudioEngine::stopNote(int note) {
    std::lock_guard<std::mutex> lock(mLock);
    if (mSynth) {
        fluid_synth_noteoff(mSynth, 0, note);
    }
}

void AudioEngine::sendMidiControlChange(int controller, int value) {
    std::lock_guard<std::mutex> lock(mLock);
    if (mSynth) {
        // Channel 0, Controller (e.g. 64 for sustain), Value
        fluid_synth_cc(mSynth, 0, controller, value);
    }
}

// =============================================================
// 乐器管理
// =============================================================

int AudioEngine::getInstrumentCount() {
    std::lock_guard<std::mutex> lock(mLock);
    return (int)mInstruments.size();
}

const char* AudioEngine::getInstrumentName(int index) {
    // 注意：这里不需要加锁，因为 mInstruments 只在 load 时修改
    // 但为了绝对安全，还是加上锁比较好，或者复制字符串
    // 这里的简单实现假设 UI 不会在 loadSoundFont 的同时疯狂刷新列表
    std::lock_guard<std::mutex> lock(mLock);
    if (index < 0 || index >= mInstruments.size()) return "Unknown";

    // 返回 vector 中存储的 string 的指针
    return mInstruments[index].name.c_str();
}

void AudioEngine::setInstrument(int index) {
    std::lock_guard<std::mutex> lock(mLock);
    if (!mSynth) return;

    if (index >= 0 && index < mInstruments.size()) {
        // 查表：获取真实的 Bank 和 Program
        int bank = mInstruments[index].bank;
        int prog = mInstruments[index].program;

        // 发送切换指令
        // Channel 0, Bank
        fluid_synth_bank_select(mSynth, 0, bank);
        // Channel 0, Program
        fluid_synth_program_change(mSynth, 0, prog);
    }
}

//  实现设置音量的函数
void AudioEngine::setMasterVolume(float gain) {
    std::lock_guard<std::mutex> lock(mLock);
    if (mSynth) {
        fluid_synth_set_gain(mSynth, gain);
    }
}

//加载sf2音源
bool AudioEngine::loadSoundFont(const char* path) {
    std::lock_guard<std::mutex> lock(mLock);

    if (!mSynth) return false;

    // 1. 如果之前加载过 SoundFont，先卸载！
    // mSoundFontId 记录了当前加载的 ID
    if (mSoundFontId != -1) {
        fluid_synth_sfunload(mSynth, mSoundFontId, 1); // 1 = reset presets
        mSoundFontId = -1;
    }

    // 2. 加载新的
    mSoundFontId = fluid_synth_sfload(mSynth, path, 1);

    if (mSoundFontId == -1) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "Failed to load SF2: %s", path);
        return false;
    }

    // 3重新扫描新库的乐器
    scanPresets();

    // 默认切换到第一个可用的乐器
    if (!mInstruments.empty()) {
        fluid_synth_bank_select(mSynth, 0, mInstruments[0].bank);
        fluid_synth_program_change(mSynth, 0, mInstruments[0].program);
    }
    // 强制开启混响发送 Channel 0, CC 91 (Reverb Level), Value 127 (Max)
    fluid_synth_cc(mSynth, 0, 91, 63);

    return true;
}

// 扫描乐器列表
void AudioEngine::scanPresets() {
    mInstruments.clear();

    if (!mSynth) return;

    // 获取当前 SF2
    fluid_sfont_t* sfont = fluid_synth_get_sfont(mSynth, 0);
    if (!sfont) {
        __android_log_print(ANDROID_LOG_WARN, TAG, "scanPresets: No SFont found");
        return;
    }

    // >>>>> 智能扫描方案 (Smart Scan) >>>>>
    // 这种方法 100% 稳定，不会崩溃，且能覆盖 99% 的 SF2 场景

    // 1. 定义我们要扫描的 Bank 列表
    // Bank 0: 标准 GM 乐器
    // Bank 128: 标准鼓组
    // Bank 1-8: 常见的 GM 变体/GS 扩展
    int banksToCheck[] = {0, 128, 1, 2, 3, 4, 5, 6, 7, 8};

    for (int b = 0; b < 10; b++) { // 遍历我们关注的 Bank
        int bank = banksToCheck[b];

        for (int prog = 0; prog < 128; prog++) { // 遍历 0-127 号乐器

            // 使用公开 API 安全查询
            // 如果这里没有乐器，它会返回 nullptr，而不会崩溃
            fluid_preset_t* preset = fluid_sfont_get_preset(sfont, bank, prog);

            if (preset != nullptr) {
                // 找到了！记录下来
                const char* name = fluid_preset_get_name(preset);
                if (!name) name = "Unknown";

                char displayName[256];
                if (bank == 128) {
                    snprintf(displayName, sizeof(displayName), "%s (Drum Kit)", name);
                } else if (bank != 0) {
                    snprintf(displayName, sizeof(displayName), "%s (Bank %d)", name, bank);
                } else {
                    // Bank 0 直接显示名字
                    snprintf(displayName, sizeof(displayName), "%s", name);
                }

                InstrumentInfo info;
                info.name = std::string(displayName);
                info.bank = bank;
                info.program = prog;

                mInstruments.push_back(info);
            }
        }
    }
    // <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<

    __android_log_print(ANDROID_LOG_INFO, TAG, "Scanned %zu presets via Smart Scan.", mInstruments.size());
}



// =============================================================
// 音频渲染回调 (最关键的部分)
// =============================================================

oboe::DataCallbackResult AudioEngine::onAudioReady(
        oboe::AudioStream *audioStream,
        void *audioData,
        int32_t numFrames) {

    float *floatData = static_cast<float *>(audioData);

    // 在音频线程中，try_lock 比 lock 更好，防止音频线程被 UI 线程卡死导致爆音
    // 如果拿不到锁，就输出静音，等待下一帧
    std::unique_lock<std::mutex> lock(mLock, std::try_to_lock);

    if (lock.owns_lock() && mSynth) {
        // FluidLite 的核心渲染函数
        // 它会自动处理所有的插值、混响、合唱
        // 参数：synth, frames, out_left, off_l, stride_l, out_right, off_r, stride_r
        fluid_synth_write_float(
                mSynth,
                numFrames,
                floatData, 0, 2, // 左声道写入偶数位置 (0, 2, 4...)
                floatData, 1, 2  // 右声道写入奇数位置 (1, 3, 5...)
        );
    } else {
        // 如果引擎未就绪或锁被占用，填充静音
        memset(floatData, 0, numFrames * audioStream->getChannelCount() * sizeof(float));
    }

    return oboe::DataCallbackResult::Continue;
}