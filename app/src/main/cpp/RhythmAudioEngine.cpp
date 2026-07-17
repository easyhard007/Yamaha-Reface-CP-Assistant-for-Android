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
    // Load 4 round-robin samples each for bass (L) and tone/tip (R)
    // filenames: front_lhlow_c3_64_rr1.wav etc.
    const char* types[] = {"front_lhlow_c3", "front_rhmid_d4"};
    int layers[] = {64, 127};
    int rrCount = 4;

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
    return true;
}

void RhythmAudioEngine::trigger(int type, float velocity) {
    // type: 0=bass(L), 1=tone/tip(R), velocity: 0-127 MIDI
    if (samples.empty()) return;
    int base = type * 8;
    int layerIdx = (velocity <= 64) ? 0 : 1;
    int rrIdx = rand() % 4;
    int idx = base + layerIdx * 4 + rrIdx;
    if (idx < 0 || idx >= (int)samples.size()) return;

    float gain = velocity / 127.0f;
    gain = gain * gain; // velocity curve

    std::lock_guard<std::mutex> lock(voiceMutex);
    voices.push_back({samples[idx].pcm.data(), samples[idx].pcm.size(), 0, gain});
}

void RhythmAudioEngine::mixAudio(float* outBuf, int32_t numFrames) {
    std::lock_guard<std::mutex> lock(voiceMutex);
    for (auto& v : voices) {
        size_t remain = v.length - v.position;
        size_t n = (size_t)numFrames < remain ? numFrames : remain;
        for (size_t i = 0; i < n; i++) {
            float s = v.data[v.position + i] * v.gain * 4.0f; // +6dB gain boost
            outBuf[i*2]   += s; // left
            outBuf[i*2+1] += s; // right
        }
        v.position += n;
    }
    // Remove finished voices
    voices.erase(std::remove_if(voices.begin(), voices.end(),
        [](const ActiveVoice& v) { return v.position >= v.length; }), voices.end());
}
