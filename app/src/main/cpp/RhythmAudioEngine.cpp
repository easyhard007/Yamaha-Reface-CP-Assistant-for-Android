#include "RhythmAudioEngine.h"
#include <fstream>
#include <cstring>
#include <cstdlib>
#include <android/log.h>

#define TAG "RhythmAudio"

static int32_t readInt32LE(const uint8_t* d) {
    return d[0] | (d[1]<<8) | (d[2]<<16) | (d[3]<<24);
}
static int16_t readInt16LE(const uint8_t* d) {
    return d[0] | (d[1]<<8);
}

RhythmAudioEngine::RhythmAudioEngine() = default;
RhythmAudioEngine::~RhythmAudioEngine() = default;

WavSample RhythmAudioEngine::loadWav(const std::string& path) {
    WavSample ws;
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "Cannot open %s", path.c_str());
        return ws;
    }
    // Read entire file
    f.seekg(0, std::ios::end);
    size_t sz = f.tellg();
    f.seekg(0, std::ios::beg);
    std::vector<uint8_t> data(sz);
    f.read((char*)data.data(), sz);

    // Parse RIFF
    if (sz < 44 || memcmp(data.data(), "RIFF", 4) || memcmp(data.data()+8, "WAVE", 4)) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "Not a WAV: %s", path.c_str());
        return ws;
    }
    size_t pos = 12;
    int bits = 16, channels = 1, sr = 44100;
    const uint8_t* pcmData = nullptr;
    size_t pcmSize = 0;

    while (pos + 8 <= sz) {
        char cid[5] = {};
        memcpy(cid, data.data()+pos, 4);
        uint32_t csz = readInt32LE(data.data()+pos+4);
        pos += 8;
        if (strcmp(cid, "fmt ") == 0) {
            if (csz >= 16) {
                channels = readInt16LE(data.data()+pos+2);
                sr = readInt32LE(data.data()+pos+4);
                bits = readInt16LE(data.data()+pos+14);
            }
        } else if (strcmp(cid, "data") == 0) {
            pcmData = data.data() + pos;
            pcmSize = csz;
        }
        pos += csz;
    }

    if (!pcmData || pcmSize == 0) return ws;

    // Convert to float mono
    size_t sampleCount = pcmSize / (bits/8 * channels);
    ws.sampleRate = sr;
    ws.pcm.resize(sampleCount);

    if (bits == 16) {
        const int16_t* s16 = (const int16_t*)pcmData;
        for (size_t i = 0; i < sampleCount; i++) {
            float v = s16[i * channels] / 32768.0f; // mono: take first channel
            ws.pcm[i] = v;
        }
    } else if (bits == 24) {
        for (size_t i = 0; i < sampleCount; i++) {
            int32_t v = (pcmData[i*channels*3] | (pcmData[i*channels*3+1]<<8) | (pcmData[i*channels*3+2]<<16));
            if (v & 0x800000) v |= 0xFF000000;
            ws.pcm[i] = v / 8388608.0f;
        }
    }

    __android_log_print(ANDROID_LOG_INFO, TAG, "Loaded %s: %zu samples %dHz", path.c_str(), sampleCount, sr);
    return ws;
}

bool RhythmAudioEngine::loadSamples(const std::string& wavDir) {
    // Load 8 round-robin samples each for bass (L) and tone/tip (R)
    // filenames: front_lhlow_c3_64_rr1.wav etc.
    const char* types[] = {"front_lhlow_c3", "front_rhmid_d4"};
    int layers[] = {64, 127};
    int rrCount = 8;

    for (auto t : types) {
        for (auto l : layers) {
            for (int r = 1; r <= rrCount; r++) {
                char buf[256];
                snprintf(buf, sizeof(buf), "%s/%s_%d_rr%d.wav", wavDir.c_str(), t, l, r);
                auto ws = loadWav(buf);
                if (ws.pcm.empty()) return false;
                samples.push_back(std::move(ws));
            }
        }
    }
    __android_log_print(ANDROID_LOG_INFO, TAG, "Loaded %zu samples", samples.size());
    // 初始化混响, 默认参数与 AudioEngine 一致
    setReverb(0.85f, 0.70f);
    return true;
}

bool RhythmAudioEngine::loadSlapSamples(const std::string& wavDir) {
    for (int r = 1; r <= 8; r++) {
        char buf[256];
        snprintf(buf, sizeof(buf), "%s/front_lhslap_e3_127_rr%d.wav", wavDir.c_str(), r);
        auto ws = loadWav(buf);
        if (ws.pcm.empty()) return false;
        slapSamples.push_back(std::move(ws));
    }
    __android_log_print(ANDROID_LOG_INFO, TAG, "Loaded %zu slap samples", slapSamples.size());
    return true;
}

void RhythmAudioEngine::triggerSlap(float velocity) {
    if (slapSamples.empty()) return;
    int rrIdx = rand() % 8;
    float gain = velocity / 127.0f;
    gain = gain * gain;
    std::lock_guard<std::mutex> lock(voiceMutex);
    voices.push_back({slapSamples[rrIdx].pcm.data(), slapSamples[rrIdx].pcm.size(), 0, gain});
}

void RhythmAudioEngine::trigger(int type, float velocity) {
    // type: 0=bass(L), 1=tone/tip(R), velocity: 0-127 MIDI
    if (samples.empty()) { __android_log_print(ANDROID_LOG_WARN, "RhythmAudio", "trigger: no samples loaded"); return; }
    int base = type * 16;  // 2 layers × 8 rr
    int layerIdx = (velocity <= 64) ? 0 : 1;
    int rrIdx = rand() % 8;
    int idx = base + layerIdx * 8 + rrIdx;
    if (idx < 0 || idx >= (int)samples.size()) return;

    float gain = velocity / 127.0f;
    gain = gain * gain; // velocity curve

    std::lock_guard<std::mutex> lock(voiceMutex);
    voices.push_back({samples[idx].pcm.data(), samples[idx].pcm.size(), 0, gain});
}

void RhythmAudioEngine::applyBassEQ(float* buf, int32_t numFrames) {
    // 35Hz +1dB, Q=0.375 (bass only)
    const float b0 = 1.00076f, b1 = -1.98751f, b2 = 0.98676f, a1 = -1.98751f, a2 = 0.98753f;
    for (int32_t i = 0; i < numFrames; i++) {
        float xL = buf[i * 2], xR = buf[i * 2 + 1];
        float yL = b0 * xL + b1 * bassEq_x1L + b2 * bassEq_x2L - a1 * bassEq_y1L - a2 * bassEq_y2L;
        bassEq_x2L = bassEq_x1L; bassEq_x1L = xL;
        bassEq_y2L = bassEq_y1L; bassEq_y1L = yL;
        float yR = b0 * xR + b1 * bassEq_x1R + b2 * bassEq_x2R - a1 * bassEq_y1R - a2 * bassEq_y2R;
        bassEq_x2R = bassEq_x1R; bassEq_x1R = xR;
        bassEq_y2R = bassEq_y1R; bassEq_y1R = yR;
        buf[i * 2] = yL; buf[i * 2 + 1] = yR;
    }
}

void RhythmAudioEngine::applyBassHiCutEQ(float* buf, int32_t numFrames) {
    // Bell EQ: f0=4kHz, Q=1.0, gain=-1dB
    const float b0 = 0.9758f, b1 = -1.3093f, b2 = 0.5795f, a1 = -1.3093f, a2 = 0.5553f;
    for (int32_t i = 0; i < numFrames; i++) {
        float xL = buf[i * 2], xR = buf[i * 2 + 1];
        float yL = b0 * xL + b1 * bassHiEq_x1L + b2 * bassHiEq_x2L - a1 * bassHiEq_y1L - a2 * bassHiEq_y2L;
        bassHiEq_x2L = bassHiEq_x1L; bassHiEq_x1L = xL;
        bassHiEq_y2L = bassHiEq_y1L; bassHiEq_y1L = yL;
        float yR = b0 * xR + b1 * bassHiEq_x1R + b2 * bassHiEq_x2R - a1 * bassHiEq_y1R - a2 * bassHiEq_y2R;
        bassHiEq_x2R = bassHiEq_x1R; bassHiEq_x1R = xR;
        bassHiEq_y2R = bassHiEq_y1R; bassHiEq_y1R = yR;
        buf[i * 2] = yL; buf[i * 2 + 1] = yR;
    }
}

void RhythmAudioEngine::processBassReverb(float* buf, int32_t numFrames) {
    applyBassEQ(buf, numFrames);     // +10dB @ 60Hz
    applyBassHiCutEQ(buf, numFrames); // -10dB @ 4kHz
    if (!bassReverbInited) {
        bassReverb.init(44100);
        bassReverb.setRoomSize(0.85f);
        bassReverb.setMix(0.07f);
        bassReverb.setDamp(0.6f);
        bassReverb.setLowDamp(0.2f);
        bassReverbInited = true;
    }
    bassReverb.process(buf, numFrames);
}

void RhythmAudioEngine::setReverb(float roomSize, float level) {
    if (!reverbInited) {
        reverb.init(44100);
        reverbInited = true;
    }
    reverb.setRoomSize(roomSize);
    // 干湿比 ≈ 6:1: UI level 0-1 映射到 wet mix 0-0.20
    reverb.setMix(level * 0.10f);
    // 衰减高频, 让混响尾巴更暖
    reverb.setDamp(0.6f);
    reverb.setLowDamp(0.2f); // 衰减 0-100Hz 低频混响
}

void RhythmAudioEngine::mixAudio(float* outBuf, int32_t numFrames) {
    // 确保 temp buffer 足够大 (音频回调中不分配内存)
    size_t needed = (size_t)numFrames * 2;
    if (tempBuf.size() < needed) tempBuf.resize(needed);

    // 先清零 temp buffer
    std::fill(tempBuf.begin(), tempBuf.begin() + needed, 0.0f);

    {
        std::lock_guard<std::mutex> lock(voiceMutex);
        float gain = masterGain.load();
        for (auto& v : voices) {
            size_t remain = v.length - v.position;
            size_t n = (size_t)numFrames < remain ? numFrames : remain;
            for (size_t i = 0; i < n; i++) {
                float s = v.data[v.position + i] * v.gain * gain;
                tempBuf[i*2]   += s;
                tempBuf[i*2+1] += s;
            }
            v.position += n;
        }
        // Remove finished voices
        voices.erase(std::remove_if(voices.begin(), voices.end(),
            [](const ActiveVoice& v) { return v.position >= v.length; }), voices.end());
    }

    // 低频 Bell EQ: 60Hz +10dB, Q≈0.375 (20-180Hz)
    applyLowBellEQ(tempBuf.data(), numFrames);

    // 箱鼓混响 (仅处理 Cajon 干声, 不影响合成器)
    if (reverbInited) {
        reverb.process(tempBuf.data(), numFrames);
    }

    // 将处理后的 Cajon (dry+wet) 混入输出
    for (int32_t i = 0; i < numFrames * 2; i++) {
        outBuf[i] += tempBuf[i];
    }
}

// Biquad peaking EQ: f0=60Hz, fs=44100, Q=0.375, gain=+10dB (Cajon)
void RhythmAudioEngine::applyLowBellEQ(float* buf, int32_t numFrames) {
    const float b0 = 1.01379f;
    const float b1 = -1.98719f;
    const float b2 = 0.97352f;
    const float a1 = -1.98719f;
    const float a2 = 0.98726f;

    for (int32_t i = 0; i < numFrames; i++) {
        float xL = buf[i * 2];
        float xR = buf[i * 2 + 1];

        // Left channel
        float yL = b0 * xL + b1 * eq_x1L + b2 * eq_x2L - a1 * eq_y1L - a2 * eq_y2L;
        eq_x2L = eq_x1L; eq_x1L = xL;
        eq_y2L = eq_y1L; eq_y1L = yL;

        // Right channel
        float yR = b0 * xR + b1 * eq_x1R + b2 * eq_x2R - a1 * eq_y1R - a2 * eq_y2R;
        eq_x2R = eq_x1R; eq_x1R = xR;
        eq_y2R = eq_y1R; eq_y1R = yR;

        buf[i * 2]     = yL;
        buf[i * 2 + 1] = yR;
    }
}
