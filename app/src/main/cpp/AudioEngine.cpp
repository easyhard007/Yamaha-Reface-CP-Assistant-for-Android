#include "AudioEngine.h"
#include <android/log.h>

// TSF 的实现必须在一个 cpp 文件中定义一次
#define TSF_IMPLEMENTATION
#include "tsf.h"

#define TAG "AudioEngine"

bool AudioEngine::init(const char* sf2Path) {
    // 加载 SF2 文件
    g_TinySoundFont = tsf_load_filename(sf2Path);
    if (!g_TinySoundFont) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "Could not load SoundFont: %s", sf2Path);
        return false;
    }

    // 设置输出模式 (标准立体声)
    tsf_set_output(g_TinySoundFont, TSF_STEREO_INTERLEAVED, 48000, 0);
    __android_log_print(ANDROID_LOG_INFO, TAG, "SoundFont loaded successfully");

    // >>>>> 初始化混响 >>>>>
    mReverb.init(48000);
    mReverb.setRoomSize(1.0f); // 设置房间大小 (0.0 - 1.0)
    mReverb.setMix(0.2f);      // 设置混响浓度 (0.0 - 1.0)

    return true;
}

void AudioEngine::start() {
    oboe::AudioStreamBuilder builder;
    builder.setDirection(oboe::Direction::Output)
            ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
            ->setSharingMode(oboe::SharingMode::Exclusive)
            ->setFormat(oboe::AudioFormat::Float)
            ->setChannelCount(oboe::ChannelCount::Stereo)
            ->setSampleRate(48000)
            ->setCallback(this); // 设置回调

    oboe::Result result = builder.openStream(stream);
    if (result != oboe::Result::OK) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "Failed to open stream");
        return;
    }

    // 启动音频流
    stream->requestStart();
}

void AudioEngine::stop() {
    if (stream) {
        stream->stop();
        stream->close();
    }
}

void AudioEngine::playNote(int note, int velocity) {
    std::lock_guard<std::mutex> lock(mLock);

    if (g_TinySoundFont) {
        // 改用 tsf_channel_note_on
        // 参数：实例, 通道号(0-15), 音高, 力度
        tsf_channel_note_on(g_TinySoundFont, 0, note, velocity / 127.0f);
    }
}

void AudioEngine::stopNote(int note) {
    std::lock_guard<std::mutex> lock(mLock);

    if (g_TinySoundFont) {
        // 改用 tsf_channel_note_off
        tsf_channel_note_off(g_TinySoundFont, 0, note);
    }
}

void AudioEngine::sendMidiControlChange(int controller, int value) {
    // 虽然渲染线程去掉了锁，但这种状态修改操作还是建议加上锁，防止和 playNote 冲突
    std::lock_guard<std::mutex> lock(mLock);

    if (g_TinySoundFont) {
        // 参数：实例, 通道(0), 控制器编号, 值
        tsf_channel_midi_control(g_TinySoundFont, 0, controller, value);
    }
}

int AudioEngine::getInstrumentCount() {
    if (!g_TinySoundFont) return 0;
    return tsf_get_presetcount(g_TinySoundFont);
}

const char* AudioEngine::getInstrumentName(int index) {
    if (!g_TinySoundFont) return "Unknown";
    return tsf_get_presetname(g_TinySoundFont, index);
}

void AudioEngine::setInstrument(int index) {
    // 必须加锁！否则在切换乐器的瞬间如果音频线程在读取数据，会崩
    std::lock_guard<std::mutex> lock(mLock);

    if (g_TinySoundFont) {
        // 将 MIDI 通道 0 (Channel 0) 的预设设置为 index
        // 我们目前的 nativeNoteOn 默认是在 Channel 0 上播放的
        tsf_channel_set_presetindex(g_TinySoundFont, 0, index);
    }
}

oboe::DataCallbackResult AudioEngine::onAudioReady(
        oboe::AudioStream *audioStream,
        void *audioData,
        int32_t numFrames) {

    float *floatData = static_cast<float *>(audioData);

    // 1. 先让 TSF 渲染干声 (Dry Signal) 到 floatData 里
    if (g_TinySoundFont) {
        // TSF 会把生成的波形写入 floatData
        tsf_render_float(g_TinySoundFont, floatData, numFrames, 0);

        // 2. >>>>> 关键步骤：混响处理 >>>>>
        // mReverb.process 会读取 floatData 里的干声，计算混响，
        // 然后把 (干声 + 混响) 混合后的结果 **写回** floatData
        mReverb.process(floatData, numFrames);
        // <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<

    } else {
        // 如果引擎没准备好，输出静音
        memset(floatData, 0, numFrames * audioStream->getChannelCount() * sizeof(float));
    }

    return oboe::DataCallbackResult::Continue;
}