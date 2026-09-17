#include "DrumLoopEngine.h"
#include "TempoStream.h"
#include "SignalsmithStream.h"
#include <android/log.h>

// 每次 renderFeed/sgsmFeed 最多渲染的帧数 (1 秒输出)
static constexpr size_t kRenderCapFrames = 48000;
// 循环边界渐变帧数
static constexpr double kFade = 256.0;

static std::shared_ptr<DrumLoopEngine::Loop> findOrCreateLocked(
        std::map<std::string, std::shared_ptr<DrumLoopEngine::Loop>>& loops,
        const std::string& name) {
    auto it = loops.find(name);
    if (it != loops.end()) return it->second;
    auto loop = std::make_shared<DrumLoopEngine::Loop>();
    loops[name] = loop;
    return loop;
}

// ================= 高质量 (sbsms) =================

bool DrumLoopEngine::renderStart(const std::string& name, int sampleRate, float ratio, int gen) {
    if (name.empty()) return false;
    std::lock_guard<std::mutex> lk(mLock);
    auto loop = findOrCreateLocked(mLoops, name);
    loop->sampleRate = sampleRate > 0 ? sampleRate : 48000;
    loop->ratio = ratio;
    loop->renderGen = gen;
    if (mActiveLoop == loop || loop->sgsm) {
        // 播放中或 sgsm 活跃: 高质量渲染进入 pending, 完整后在循环边界切换
        loop->pendingBuf = std::make_shared<std::vector<int16_t>>();
        loop->pendingStream = std::make_shared<TempoStream>(loop->sampleRate, ratio);
        loop->pendingComplete = false;
    } else {
        loop->buf = std::make_shared<std::vector<int16_t>>();
        loop->stream = std::make_shared<TempoStream>(loop->sampleRate, ratio);
        loop->complete = false;
    }
    return true;
}

int64_t DrumLoopEngine::renderFeed(const std::string& name, const int16_t* pcm, int32_t totalSamples, int gen) {
    if (!pcm || totalSamples <= 0) return 0;
    std::shared_ptr<TempoStream> st;
    std::shared_ptr<std::vector<int16_t>> dst;
    {
        std::lock_guard<std::mutex> lk(mLock);
        auto it = mLoops.find(name);
        if (it == mLoops.end()) return 0;
        auto loop = it->second;
        if (loop->renderGen != gen) return -1;
        st = loop->pendingStream ? loop->pendingStream : loop->stream;
        dst = loop->pendingStream ? loop->pendingBuf : loop->buf;
        if (!st || !dst) return 0;
        st->feed(pcm, (size_t)totalSamples / 2);
    }
    std::vector<int16_t> tmp(kRenderCapFrames * 2);
    size_t n = st->render(tmp.data(), kRenderCapFrames);
    if (n > 0) {
        std::lock_guard<std::mutex> lk(mLock);
        dst->insert(dst->end(), tmp.data(), tmp.data() + n * 2);
    }
    std::lock_guard<std::mutex> lk(mLock);
    return (int64_t)(dst->size() / 2);
}

bool DrumLoopEngine::renderFinish(const std::string& name, int gen) {
    std::shared_ptr<TempoStream> st;
    std::shared_ptr<std::vector<int16_t>> dst;
    bool pendingMode = false;
    {
        std::lock_guard<std::mutex> lk(mLock);
        auto it = mLoops.find(name);
        if (it == mLoops.end()) return false;
        auto loop = it->second;
        if (loop->renderGen != gen) return false;
        pendingMode = (bool)loop->pendingStream;
        st = loop->pendingStream ? loop->pendingStream : loop->stream;
        dst = loop->pendingStream ? loop->pendingBuf : loop->buf;
        if (!st || !dst) return false;
        st->finishInput();
    }
    std::vector<int16_t> tmp(kRenderCapFrames * 2);
    int guard = 0;
    while (guard++ < 20000) {
        size_t n = st->render(tmp.data(), kRenderCapFrames);
        if (n == 0) break;
        std::lock_guard<std::mutex> lk(mLock);
        dst->insert(dst->end(), tmp.data(), tmp.data() + n * 2);
    }
    {
        std::lock_guard<std::mutex> lk(mLock);
        auto it = mLoops.find(name);
        if (it == mLoops.end() || it->second->renderGen != gen) return false;
        auto loop = it->second;
        if (pendingMode) {
            loop->pendingComplete = true;
            loop->pendingStream.reset();
            __android_log_print(ANDROID_LOG_INFO, "DrumLoop",
                                "sbsms finished (pending) %s ratio=%.3f frames=%zu",
                                name.c_str(), loop->ratio, dst->size() / 2);
        } else {
            loop->complete = true;
            loop->stream.reset();
            __android_log_print(ANDROID_LOG_INFO, "DrumLoop",
                                "sbsms finished %s ratio=%.3f frames=%zu",
                                name.c_str(), loop->ratio, dst->size() / 2);
        }
    }
    return true;
}

// ================= 低质量低延迟 (Signalsmith) =================

bool DrumLoopEngine::sgsmStart(const std::string& name, int sampleRate, float ratio, int gen) {
    if (name.empty()) return false;
    std::lock_guard<std::mutex> lk(mLock);
    auto loop = findOrCreateLocked(mLoops, name);
    loop->sampleRate = sampleRate > 0 ? sampleRate : 48000;
    loop->ratio = ratio;
    loop->sgsmGen = gen;
    // 若 sbsms 渲染正在进行 (直接模式), 将其转入 pending, buf 让给 sgsm
    if (loop->stream) {
        loop->pendingStream = loop->stream;
        loop->pendingBuf = loop->buf;
        loop->stream.reset();
        loop->pendingComplete = false;
    }
    loop->sgsm = std::make_shared<SignalsmithStream>(loop->sampleRate, ratio);
    loop->buf = std::make_shared<std::vector<int16_t>>();
    loop->complete = false;
    return true;
}

int64_t DrumLoopEngine::sgsmFeed(const std::string& name, const int16_t* pcm, int32_t totalSamples, int gen) {
    if (!pcm || totalSamples <= 0) return 0;
    std::shared_ptr<SignalsmithStream> sgsm;
    std::shared_ptr<std::vector<int16_t>> dst;
    {
        std::lock_guard<std::mutex> lk(mLock);
        auto it = mLoops.find(name);
        if (it == mLoops.end()) return 0;
        auto loop = it->second;
        if (loop->sgsmGen != gen) return -1;
        sgsm = loop->sgsm;
        dst = loop->buf;
        if (!sgsm || !dst) return 0;
        sgsm->feed(pcm, (size_t)totalSamples / 2);
    }
    std::vector<int16_t> tmp(kRenderCapFrames * 2);
    size_t n = sgsm->render(tmp.data(), kRenderCapFrames);
    if (n > 0) {
        std::lock_guard<std::mutex> lk(mLock);
        dst->insert(dst->end(), tmp.data(), tmp.data() + n * 2);
    }
    std::lock_guard<std::mutex> lk(mLock);
    return (int64_t)(dst->size() / 2);
}

bool DrumLoopEngine::sgsmFinish(const std::string& name, int gen) {
    std::shared_ptr<SignalsmithStream> sgsm;
    std::shared_ptr<std::vector<int16_t>> dst;
    {
        std::lock_guard<std::mutex> lk(mLock);
        auto it = mLoops.find(name);
        if (it == mLoops.end()) return false;
        auto loop = it->second;
        if (loop->sgsmGen != gen) return false;
        sgsm = loop->sgsm;
        dst = loop->buf;
        if (!sgsm || !dst) return false;
        sgsm->finishInput();
    }
    std::vector<int16_t> tmp(kRenderCapFrames * 2);
    int guard = 0;
    while (guard++ < 20000) {
        size_t n = sgsm->render(tmp.data(), kRenderCapFrames);
        if (n == 0) break;
        std::lock_guard<std::mutex> lk(mLock);
        dst->insert(dst->end(), tmp.data(), tmp.data() + n * 2);
    }
    {
        std::lock_guard<std::mutex> lk(mLock);
        auto it = mLoops.find(name);
        if (it == mLoops.end() || it->second->sgsmGen != gen) return false;
        it->second->complete = true;
        it->second->sgsm.reset();
        __android_log_print(ANDROID_LOG_INFO, "DrumLoop",
                            "sgsm finished %s ratio=%.3f frames=%zu",
                            name.c_str(), it->second->ratio, dst->size() / 2);
    }
    return true;
}

void DrumLoopEngine::renderCancelAll() {
    std::lock_guard<std::mutex> lk(mLock);
    for (auto& kv : mLoops) {
        kv.second->stream.reset();
        kv.second->sgsm.reset();
        kv.second->pendingStream.reset();
        kv.second->pendingBuf.reset();
        kv.second->pendingComplete = false;
    }
    __android_log_print(ANDROID_LOG_INFO, "DrumLoop", "renderCancelAll");
}

void DrumLoopEngine::clear() {
    std::lock_guard<std::mutex> lk(mLock);
    mLoops.clear();
    mActiveLoop.reset();
    mActivePos = 0.0;
}

void DrumLoopEngine::play(const std::string& name) {
    std::lock_guard<std::mutex> lk(mLock);
    auto it = mLoops.find(name);
    if (it == mLoops.end()) return;
    mActiveLoop = it->second;
    mActivePos = 0.0;
}

void DrumLoopEngine::stop() {
    std::lock_guard<std::mutex> lk(mLock);
    mActiveLoop.reset();
    mActivePos = 0.0;
}

void DrumLoopEngine::mixAudio(float* outBuf, int32_t numFrames, int32_t deviceSampleRate) {
    const float vol = mVolume.load();
    if (vol <= 0.0f || !outBuf || numFrames <= 0) return;
    if (deviceSampleRate <= 0) deviceSampleRate = 48000;

    std::lock_guard<std::mutex> lk(mLock);
    std::shared_ptr<Loop> loop = mActiveLoop;
    if (!loop || !loop->buf || loop->buf->empty()) return;

    const std::vector<int16_t>* cur = loop->buf.get();
    int64_t totalFrames = (int64_t)(cur->size() / 2);
    if (totalFrames < 2) return;

    // 起播水位: 渲染未完成时, 等缓冲积累到 ~0.15s 再开始播放,
    // 避免渲染批次抖动导致播放断续 (低延迟引擎填充快, 之后缓冲持续超前)
    if (!loop->complete) {
        const int64_t watermark = (int64_t)loop->sampleRate * 15 / 100;
        if (totalFrames < watermark) return;
    }

    double pos = mActivePos;
    const double step = (double)loop->sampleRate / (double)deviceSampleRate * (double)mRate.load();
    const float kScale = 1.0f / 32768.0f;

    for (int32_t i = 0; i < numFrames; i++) {
        if (pos >= (double)totalFrames) {
            // 本遍播完: 有 pending (高质量重渲染完成) → 切换; 完整 → 循环; 否则停在末尾
            if (loop->pendingComplete && loop->pendingBuf && !loop->pendingBuf->empty()) {
                loop->buf = loop->pendingBuf;
                loop->pendingBuf.reset();
                loop->pendingComplete = false;
                loop->complete = true;
                cur = loop->buf.get();
                totalFrames = (int64_t)(cur->size() / 2);
                pos = 0.0;
                if (totalFrames < 2) break;
            } else if (!loop->complete) {
                pos = (double)totalFrames;  // 渲染未完成: 停在末尾等待
                break;
            } else {
                pos -= (double)totalFrames;
            }
        }

        int64_t ip = (int64_t)pos;
        int64_t ip2 = ip + 1;
        if (ip2 >= totalFrames) ip2 -= totalFrames;

        double frac = pos - (double)ip;
        if (frac < 0.0) frac = 0.0;
        if (frac > 1.0) frac = 1.0;

        const size_t b0 = (size_t)ip * 2;
        const size_t b1 = (size_t)ip2 * 2;
        float li = ((*cur)[b0] * kScale) + (((*cur)[b1] * kScale) - ((*cur)[b0] * kScale)) * (float)frac;
        float ri = ((*cur)[b0 + 1] * kScale) + (((*cur)[b1 + 1] * kScale) - ((*cur)[b0 + 1] * kScale)) * (float)frac;

        // 循环边界渐变 (仅完整缓冲)
        if (loop->complete) {
            if (pos >= (double)totalFrames - kFade) {
                const double rem = (double)totalFrames - pos;
                li *= (float)(rem / kFade);
                ri *= (float)(rem / kFade);
            } else if (pos < kFade) {
                li *= (float)(pos / kFade);
                ri *= (float)(pos / kFade);
            }
        }

        outBuf[i * 2] += li * vol;
        outBuf[i * 2 + 1] += ri * vol;

        pos += step;
    }

    mActivePos = pos;
}
