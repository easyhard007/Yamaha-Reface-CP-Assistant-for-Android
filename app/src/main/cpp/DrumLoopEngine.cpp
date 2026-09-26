#include "DrumLoopEngine.h"
#include "TempoStream.h"
#include "SignalsmithStream.h"
#include <android/log.h>
#include <cstdio>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>

// 每次 DSP 调用最多渲染约 170 ms 音频，缩短单次 CPU burst 与共享缓冲追加锁的持有时间。
static constexpr size_t kRenderCapFrames = 8192;
// 循环边界渐变帧数
static constexpr double kFade = 256.0;

// 诊断: 输出缓冲前 0.8s 的 0.1s 窗口 RMS (检测渲染数据中的静音/缺陷)
static void logRmsProfile(const std::string& name, const std::vector<int16_t>& buf, int sampleRate) {
    if (sampleRate <= 0 || buf.size() < 4) return;
    const size_t total = buf.size() / 2;
    const size_t win = (size_t)sampleRate / 10;   // 0.1s
    const size_t count = std::min<size_t>(8, total / win);
    char msg[512];
    int off = snprintf(msg, sizeof(msg), "rms[%s]", name.c_str());
    for (size_t i = 0; i < count && off < (int)sizeof(msg) - 32; i++) {
        double s = 0;
        for (size_t j = i * win; j < (i + 1) * win && j < total; j++) {
            double x = buf[j * 2] / 32768.0;
            s += x * x;
        }
        off += snprintf(msg + off, sizeof(msg) - off, " %.2f", sqrt(s / win));
    }
    __android_log_print(ANDROID_LOG_INFO, "DrumLoop", "%s", msg);
}

static std::shared_ptr<DrumLoopEngine::Loop> findOrCreateLocked(
        std::map<std::string, std::shared_ptr<DrumLoopEngine::Loop>>& loops,
        const std::string& name) {
    auto it = loops.find(name);
    if (it != loops.end()) return it->second;
    auto loop = std::make_shared<DrumLoopEngine::Loop>();
    loop->name = name;
    loops[name] = loop;
    return loop;
}

// 把已经达到起播水位的 SBSMS pending 缓冲发布为主缓冲。
// 调用者必须持有 mLock；旧对象通过引用带出锁外析构，避免阻塞 Oboe 回调。
static bool publishPendingLocked(
        const std::shared_ptr<DrumLoopEngine::Loop>& loop,
        std::shared_ptr<std::vector<int16_t>>& retiredBuf,
        std::shared_ptr<SignalsmithStream>& retiredSgsm) {
    if (!loop->pendingBuf || loop->pendingBuf->empty()) return false;

    retiredBuf = std::move(loop->buf);
    retiredSgsm = std::move(loop->sgsm);
    loop->sgsmGen = -1;
    loop->sgsmDrainStarted = false;
    loop->buf = std::move(loop->pendingBuf);
    loop->bufGen = loop->renderGen;
    loop->complete = loop->pendingComplete;
    loop->pendingComplete = false;
    loop->pendingPlayable = false;
    loop->promoteAtWrap = false;
    return true;
}

// ================= 高质量 (sbsms) =================

bool DrumLoopEngine::renderStart(const std::string& name, int sampleRate, float ratio, int gen) {
    if (name.empty()) return false;
    const int safeSampleRate = sampleRate > 0 ? sampleRate : 48000;
    // DSP 构造与旧对象析构都可能分配/释放较多内存，不能占用音频回调共用的 mLock。
    auto nextBuf = std::make_shared<std::vector<int16_t>>();
    auto nextStream = std::make_shared<TempoStream>(safeSampleRate, ratio);
    std::shared_ptr<std::vector<int16_t>> retiredBuf;
    std::shared_ptr<std::vector<int16_t>> retiredPendingBuf;
    std::shared_ptr<TempoStream> retiredStream;
    std::shared_ptr<TempoStream> retiredPendingStream;
    {
        std::lock_guard<std::mutex> lk(mLock);
        auto loop = findOrCreateLocked(mLoops, name);
        loop->sampleRate = safeSampleRate;
        loop->ratio = ratio;
        loop->renderGen = gen;
        loop->sbsmDrainStarted = false;
        loop->pendingPlayable = false;
        loop->promoteAtWrap = false;
        if (isLoopActive(loop) || loop->sgsm) {
            // 播放中或 sgsm 活跃: 高质量渲染进入 pending, 完整后在循环边界切换
            retiredPendingBuf = std::move(loop->pendingBuf);
            retiredPendingStream = std::move(loop->pendingStream);
            loop->pendingBuf = std::move(nextBuf);
            loop->pendingStream = std::move(nextStream);
            loop->pendingComplete = false;
        } else {
            // 直接模式: 清掉可能残留的旧 pending, 避免陈旧缓冲被误用
            retiredPendingStream = std::move(loop->pendingStream);
            retiredPendingBuf = std::move(loop->pendingBuf);
            retiredBuf = std::move(loop->buf);
            retiredStream = std::move(loop->stream);
            loop->pendingComplete = false;
            loop->buf = std::move(nextBuf);
            loop->bufGen = gen;
            loop->stream = std::move(nextStream);
            loop->complete = false;
        }
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
        // promotePending 后 pendingBuf 可能为空: 渲染结果追加到播放缓冲
        dst = (loop->pendingStream && loop->pendingBuf) ? loop->pendingBuf : loop->buf;
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

int64_t DrumLoopEngine::renderFinishStep(const std::string& name, int gen) {
    std::shared_ptr<TempoStream> st;
    std::shared_ptr<std::vector<int16_t>> dst;
    std::shared_ptr<std::vector<int16_t>> retiredBuf;
    std::shared_ptr<SignalsmithStream> retiredSgsm;
    bool pendingMode = false;
    bool drainDone = false;
    {
        std::lock_guard<std::mutex> lk(mLock);
        auto it = mLoops.find(name);
        if (it == mLoops.end()) return -1;
        auto loop = it->second;
        if (loop->renderGen != gen) return -1;
        pendingMode = (bool)loop->pendingStream;
        st = loop->pendingStream ? loop->pendingStream : loop->stream;
        // promotePending 后 pendingBuf 可能为空: 渲染结果追加到播放缓冲
        dst = (loop->pendingStream && loop->pendingBuf) ? loop->pendingBuf : loop->buf;
        if (!st || !dst) return 0;  // 已排空完成
        if (!loop->sbsmDrainStarted) {
            st->finishInput();
            loop->sbsmDrainStarted = true;
        }
    }
    // 渲染一步 (锁外)
    std::vector<int16_t> tmp(kRenderCapFrames * 2);
    size_t n = st->render(tmp.data(), kRenderCapFrames);
    if (n > 0) {
        std::lock_guard<std::mutex> lk(mLock);
        dst->insert(dst->end(), tmp.data(), tmp.data() + n * 2);
    }
    {
        std::lock_guard<std::mutex> lk(mLock);
        auto it = mLoops.find(name);
        if (it == mLoops.end() || it->second->renderGen != gen) return -1;
        auto loop = it->second;
        if (n == 0) {
            drainDone = true;
            if (pendingMode && loop->pendingBuf) {
                loop->pendingStream.reset();
                loop->pendingComplete = true;
                loop->pendingPlayable = true;
                if (!isLoopActive(loop)) {
                    publishPendingLocked(loop, retiredBuf, retiredSgsm);
                    __android_log_print(ANDROID_LOG_INFO, "DrumLoop",
                                        "sbsms drained and published %s frames=%zu",
                                        name.c_str(), dst->size() / 2);
                } else {
                    __android_log_print(ANDROID_LOG_INFO, "DrumLoop",
                                        "sbsms drained (pending) %s frames=%zu",
                                        name.c_str(), dst->size() / 2);
                }
            } else {
                // 直接模式, 或 pending 已被 promote 到播放缓冲
                loop->complete = true;
                loop->stream.reset();
                loop->pendingStream.reset();
                __android_log_print(ANDROID_LOG_INFO, "DrumLoop",
                                    "sbsms drained %s frames=%zu",
                                    name.c_str(), dst->size() / 2);
            }
            logRmsProfile(name, *dst, loop->sampleRate);
        }
    }
    return drainDone ? 0 : (int64_t)n;
}

bool DrumLoopEngine::promotePending(const std::string& name) {
    std::lock_guard<std::mutex> lk(mLock);
    auto it = mLoops.find(name);
    if (it == mLoops.end()) return false;
    auto loop = it->second;
    if (!loop->pendingBuf || loop->pendingBuf->empty()) return false;
    // pending 已至少预热 0.5 秒。下一次显式 play 可以直接从它起播。
    loop->pendingPlayable = true;

    // 若当前槽已经在播放同一批次的低质量主缓冲，也允许在本轮回卷时升级。
    // BPM 预备期间仍在播放旧速度时，bufGen 不同，因此绝不会提前换上新速度缓冲。
    const Slot& current = mSlots[mCurSlot];
    const bool currentUsesSameGenerationFallback =
        current.loop == loop && current.buf == loop->buf &&
        current.bufGen == loop->renderGen && loop->bufGen == loop->renderGen;
    loop->promoteAtWrap = currentUsesSameGenerationFallback;
    __android_log_print(ANDROID_LOG_INFO, "DrumLoop",
                        "pending playable %s frames=%zu mode=%s",
                        name.c_str(), loop->pendingBuf->size() / 2,
                        currentUsesSameGenerationFallback ? "wrap" : "next-play");
    return true;
}

// ================= 低质量低延迟 (Signalsmith) =================

bool DrumLoopEngine::sgsmStart(const std::string& name, int sampleRate, float ratio, int gen) {
    if (name.empty()) return false;
    const int safeSampleRate = sampleRate > 0 ? sampleRate : 48000;
    auto nextSgsm = std::make_shared<SignalsmithStream>(safeSampleRate, ratio);
    auto nextBuf = std::make_shared<std::vector<int16_t>>();
    std::shared_ptr<std::vector<int16_t>> retiredBuf;
    std::shared_ptr<std::vector<int16_t>> retiredPendingBuf;
    std::shared_ptr<TempoStream> retiredPendingStream;
    std::shared_ptr<SignalsmithStream> retiredSgsm;
    {
        std::lock_guard<std::mutex> lk(mLock);
        auto loop = findOrCreateLocked(mLoops, name);
        loop->sampleRate = safeSampleRate;
        loop->ratio = ratio;
        loop->sgsmGen = gen;
        retiredSgsm = std::move(loop->sgsm);
        // 若 sbsms 渲染正在进行 (直接模式), 将其转入 pending, buf 让给 sgsm
        if (loop->stream) {
            retiredPendingStream = std::move(loop->pendingStream);
            retiredPendingBuf = std::move(loop->pendingBuf);
            loop->pendingStream = std::move(loop->stream);
            loop->pendingBuf = std::move(loop->buf);
            loop->pendingComplete = false;
        } else {
            retiredBuf = std::move(loop->buf);
        }
        loop->sgsm = std::move(nextSgsm);
        loop->buf = std::move(nextBuf);
        loop->bufGen = gen;
        loop->complete = false;
        loop->sgsmDrainStarted = false;
        loop->promoteAtWrap = false;
    }
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

int64_t DrumLoopEngine::sgsmFinishStep(const std::string& name, int gen) {
    std::shared_ptr<SignalsmithStream> sgsm;
    std::shared_ptr<std::vector<int16_t>> dst;
    {
        std::lock_guard<std::mutex> lk(mLock);
        auto it = mLoops.find(name);
        if (it == mLoops.end()) return -1;
        auto loop = it->second;
        if (loop->sgsmGen != gen) return -1;
        sgsm = loop->sgsm;
        dst = loop->buf;
        if (!sgsm || !dst) return 0;
        if (!loop->sgsmDrainStarted) {
            sgsm->finishInput();
            loop->sgsmDrainStarted = true;
        }
    }
    std::vector<int16_t> tmp(kRenderCapFrames * 2);
    size_t n = sgsm->render(tmp.data(), kRenderCapFrames);
    if (n > 0) {
        std::lock_guard<std::mutex> lk(mLock);
        dst->insert(dst->end(), tmp.data(), tmp.data() + n * 2);
    }
    {
        std::lock_guard<std::mutex> lk(mLock);
        auto it = mLoops.find(name);
        if (it == mLoops.end() || it->second->sgsmGen != gen) return -1;
        auto loop = it->second;
        if (n == 0) {
            loop->complete = true;
            loop->sgsm.reset();
            __android_log_print(ANDROID_LOG_INFO, "DrumLoop",
                                "sgsm drained %s frames=%zu",
                                name.c_str(), dst->size() / 2);
            logRmsProfile(name, *dst, loop->sampleRate);
        }
    }
    return (int64_t)n;
}

void DrumLoopEngine::renderCancelAll() {
    std::vector<std::shared_ptr<TempoStream>> retiredTempoStreams;
    std::vector<std::shared_ptr<SignalsmithStream>> retiredSignalsmithStreams;
    std::vector<std::shared_ptr<std::vector<int16_t>>> retiredBuffers;
    // 常见循环包远少于此数量；预分配放在锁外，避免 detach 阶段触发容器扩容。
    retiredTempoStreams.reserve(128);
    retiredSignalsmithStreams.reserve(64);
    retiredBuffers.reserve(128);
    {
        std::lock_guard<std::mutex> lk(mLock);
        for (auto& kv : mLoops) {
            auto& loop = kv.second;
            if (loop->stream) retiredTempoStreams.emplace_back(std::move(loop->stream));
            if (loop->sgsm) retiredSignalsmithStreams.emplace_back(std::move(loop->sgsm));
            if (loop->pendingStream) retiredTempoStreams.emplace_back(std::move(loop->pendingStream));
            if (loop->pendingBuf) retiredBuffers.emplace_back(std::move(loop->pendingBuf));
            loop->pendingComplete = false;
            loop->pendingPlayable = false;
            loop->sbsmDrainStarted = false;
            loop->sgsmDrainStarted = false;
            loop->promoteAtWrap = false;
            // BPM 改变: 旧速度的渲染已失效, 非正在播放的循环彻底重置为未渲染
            // (正在播放的保留旧缓冲, 由新渲染完成后的循环边界切换接管)
            if (!isLoopActive(loop)) {
                if (loop->buf) retiredBuffers.emplace_back(std::move(loop->buf));
                loop->bufGen = -1;
                loop->complete = false;
            }
        }
    }
    // retired* 在 mLock 之外析构，避免 Oboe 回调等待 DSP/大 PCM 缓冲释放。
    __android_log_print(ANDROID_LOG_INFO, "DrumLoop", "renderCancelAll (invalidate stale renders)");
}

void DrumLoopEngine::clear() {
    std::map<std::string, std::shared_ptr<Loop>> retiredLoops;
    Slot retiredSlots[2];
    {
        std::lock_guard<std::mutex> lk(mLock);
        retiredLoops.swap(mLoops);
        retiredSlots[0] = std::move(mSlots[0]);
        retiredSlots[1] = std::move(mSlots[1]);
        mSlots[0] = Slot{};
        mSlots[1] = Slot{};
    }
    // Loop、DSP 与 PCM 的实际释放发生在播放锁之外。
}

// ================= 播放 (双槽 ping-pong) =================

void DrumLoopEngine::play(const std::string& name) {
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    int64_t nowUs = std::chrono::duration_cast<std::chrono::microseconds>(now).count();
    std::shared_ptr<std::vector<int16_t>> retiredBuf;
    std::shared_ptr<SignalsmithStream> retiredSgsm;
    {
        std::lock_guard<std::mutex> lk(mLock);
        auto it = mLoops.find(name);
        if (it == mLoops.end()) return;
        auto loop = it->second;

        // 显式切换发生在拍头：若 SBSMS 已达到预热水位（或已完整渲染），从高质量
        // pending 的第 0 帧起播。不能等低质量 buf 再循环一遍才升级。
        const bool publishedHighQuality =
            (loop->pendingPlayable || loop->pendingComplete) &&
            publishPendingLocked(loop, retiredBuf, retiredSgsm);
        const int old = mCurSlot;
        const int nu = 1 - mCurSlot;
        // 旧槽 (当前播放的) 30ms 淡出, 不立即停止
        if (mSlots[old].loop) {
            mSlots[old].fading = true;
            mSlots[old].fadeTotalUs = kSwitchFadeUs;
            mSlots[old].fadeRemainingUs = kSwitchFadeUs;
            mSlots[old].fadeInRemainingUs = 0;   // 清除未完成的淡入, 从满幅开始淡出
        }
        // 新槽立即开始 (10ms 淡入防幅度跳变)
        mSlots[nu].loop = loop;
        mSlots[nu].buf = loop->buf;   // 快照当前播放缓冲
        mSlots[nu].bufGen = loop->bufGen;
        mSlots[nu].pos = 0.0;
        mSlots[nu].gain = 1.0f;
        mSlots[nu].fadeInRemainingUs = kSwitchFadeInUs;
        mSlots[nu].fading = false;
        mCurSlot = nu;

        __android_log_print(ANDROID_LOG_INFO, "DrumLoop",
                            "play source %s: %s gen=%d complete=%d",
                            name.c_str(), publishedHighQuality ? "sbsms-pending" : "primary",
                            loop->bufGen, loop->complete ? 1 : 0);

        // 调试: 新循环缓冲头部峰值与前 16 个样本
        if (it->second->buf && !it->second->buf->empty()) {
            const std::vector<int16_t>& b = *it->second->buf;
            const int64_t n = std::min<int64_t>((int64_t)(b.size() / 2), it->second->sampleRate / 10);
            int peak = 0;
            int64_t peakAt = 0;
            for (int64_t i = 0; i < n; i++) {
                int v = std::max(std::abs((int)b[i * 2]), std::abs((int)b[i * 2 + 1]));
                if (v > peak) { peak = v; peakAt = i; }
            }
            char head[256] = {0};
            int off = 0;
            for (int64_t i = 0; i < 16 && i < (int64_t)(b.size() / 2); i++)
                off += snprintf(head + off, sizeof(head) - off, "%d ", (int)b[i * 2]);
            __android_log_print(ANDROID_LOG_INFO, "DrumLoop",
                                "play %s slot=%d frames=%lld headPeak=%d(%.2f)@%lld head=[%s]",
                                name.c_str(), nu, (long long)(b.size() / 2), peak,
                                peak / 32768.0, (long long)peakAt, head);
        }
    }
    // 记录切换触发时刻 (混音器首次出声时计算延迟)
    mTriggerUs.store(nowUs);
    mLatencyMs.store(-1);
    mFading.store(0);
    __android_log_print(ANDROID_LOG_INFO, "DrumLoop", "play %s", name.c_str());
}

void DrumLoopEngine::stop() {
    std::lock_guard<std::mutex> lk(mLock);
    mSlots[0].loop.reset();
    mSlots[1].loop.reset();
}

bool DrumLoopEngine::isPlaying() const {
    std::lock_guard<std::mutex> lk(mLock);
    return (bool)mSlots[0].loop || (bool)mSlots[1].loop;
}

void DrumLoopEngine::fadeOut(int durationMs) {
    if (durationMs <= 0) durationMs = 1;
    std::lock_guard<std::mutex> lk(mLock);
    mFadeTotalUs = (int64_t)durationMs * 1000;
    mFadeRemainingUs = mFadeTotalUs;
    mFading.store(1);
    __android_log_print(ANDROID_LOG_INFO, "DrumLoop", "fadeOut %dms", durationMs);
}

int64_t DrumLoopEngine::getAndClearLatencyMs() {
    return mLatencyMs.exchange(-1);
}

int DrumLoopEngine::getAndClearStoppedEvent() {
    return mStoppedEvent.exchange(0);
}

// ================= 混音 (双槽) =================

void DrumLoopEngine::mixAudio(float* outBuf, int32_t numFrames, int32_t deviceSampleRate) {
    float vol = mVolume.load();
    if (!outBuf || numFrames <= 0) return;
    if (deviceSampleRate <= 0) deviceSampleRate = 48000;

    std::lock_guard<std::mutex> lk(mLock);

    // 全局淡出 (停止按钮)
    float masterFade = 1.0f;
    if (mFading.load()) {
        const bool any = (bool)mSlots[0].loop || (bool)mSlots[1].loop;
        if (!any) {
            mFading.store(0);
            mStoppedEvent.store(1);
            return;
        }
        mFadeRemainingUs -= (int64_t)(1000000.0 * (double)numFrames / (double)deviceSampleRate);
        if (mFadeRemainingUs <= 0) {
            mSlots[0].loop.reset();
            mSlots[1].loop.reset();
            mFading.store(0);
            mStoppedEvent.store(1);
            __android_log_print(ANDROID_LOG_INFO, "DrumLoop", "fadeOut complete");
            return;
        }
        masterFade = (float)((double)mFadeRemainingUs / (double)mFadeTotalUs);
    }
    if (vol <= 0.0f) return;

    // 先混淡出中的旧槽, 再混当前槽
    mixSlot(mSlots[1 - mCurSlot], false, outBuf, numFrames, deviceSampleRate, vol * masterFade);
    mixSlot(mSlots[mCurSlot], true, outBuf, numFrames, deviceSampleRate, vol * masterFade);
}

void DrumLoopEngine::mixSlot(Slot& slot, bool isCurrent, float* outBuf, int32_t numFrames,
                             int32_t deviceSampleRate, float baseVol) {
    if (!slot.loop) return;
    // 播放缓冲快照: 未设置则取 loop 当前缓冲
    if (!slot.buf) slot.buf = slot.loop->buf;
    if (!slot.buf || slot.buf->empty()) return;
    const std::shared_ptr<Loop> loop = slot.loop;

    const int64_t frameUs = (int64_t)(1000000.0 * (double)numFrames / (double)deviceSampleRate);

    // 增益: 淡入阶段由 fadeIn 系数 0→1 决定, 否则用槽的常态增益
    float gain;
    if (slot.fadeInRemainingUs > 0) {
        slot.fadeInRemainingUs -= frameUs;
        const float f = 1.0f - (float)((double)std::max<int64_t>(slot.fadeInRemainingUs, 0) /
                                       (double)kSwitchFadeInUs);
        gain = baseVol * (f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f));
    } else {
        gain = slot.gain * baseVol;
    }
    if (slot.fading) {
        slot.fadeRemainingUs -= frameUs;
        if (slot.fadeRemainingUs <= 0) {
            slot.loop.reset();
            slot.fading = false;
            return;
        }
        gain *= (float)((double)slot.fadeRemainingUs / (double)slot.fadeTotalUs);
    }
    if (gain <= 0.0f) return;

    const std::vector<int16_t>* cur = slot.buf.get();
    int64_t totalFrames = (int64_t)(cur->size() / 2);
    if (totalFrames < 2) return;

    // 当前槽: 起播水位 (渲染未完成时等缓冲积累, 避免断续)
    if (isCurrent && !loop->complete) {
        const int64_t watermark = (int64_t)loop->sampleRate * 15 / 100;
        if (totalFrames < watermark) return;
    }

    double pos = slot.pos;
    const double step = (double)loop->sampleRate / (double)deviceSampleRate * (double)mRate.load();
    const float kScale = 1.0f / 32768.0f;

    for (int32_t i = 0; i < numFrames; i++) {
        if (pos >= (double)totalFrames) {
            if (!isCurrent) break;   // 淡出中的旧槽: 播完即止
            // 当前槽的回卷/切换
            if (loop->promoteAtWrap && loop->pendingBuf && !loop->pendingBuf->empty()) {
                loop->buf = loop->pendingBuf;
                slot.buf = loop->buf;
                loop->bufGen = loop->renderGen;
                slot.bufGen = loop->bufGen;
                loop->pendingBuf.reset();
                loop->promoteAtWrap = false;
                // 高质量缓冲若在切换前已排空完成, 直接标记可循环,
                // 否则保持 false 由后续按需渲染排空后置 complete
                loop->complete = loop->pendingComplete;
                loop->pendingComplete = false;
                loop->pendingPlayable = false;
                loop->sgsmGen = -1;
                cur = slot.buf.get();
                totalFrames = (int64_t)(cur->size() / 2);
                pos = 0.0;
                if (totalFrames < 2) break;
            } else if (loop->pendingComplete && loop->pendingBuf && !loop->pendingBuf->empty()) {
                loop->buf = loop->pendingBuf;
                slot.buf = loop->buf;
                loop->bufGen = loop->renderGen;
                slot.bufGen = loop->bufGen;
                loop->pendingBuf.reset();
                loop->pendingComplete = false;
                loop->pendingPlayable = false;
                loop->complete = true;
                loop->sgsmGen = -1;
                cur = slot.buf.get();
                totalFrames = (int64_t)(cur->size() / 2);
                pos = 0.0;
                if (totalFrames < 2) break;
            } else if (!loop->complete) {
                static int underrunLogSkip = 0;
                if ((underrunLogSkip++ % 100) == 0) {
                    __android_log_print(ANDROID_LOG_WARN, "DrumLoop",
                                        "underrun hold: %s buf=%lld pos=%.0f",
                                        loop->name.c_str(), (long long)totalFrames, pos);
                }
                pos = (double)totalFrames;
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

        // 回卷处渐变 (仅完整缓冲)
        if (isCurrent && loop->complete) {
            if (pos >= (double)totalFrames - kFade) {
                const double rem = (double)totalFrames - pos;
                li *= (float)(rem / kFade);
                ri *= (float)(rem / kFade);
            } else if (pos < kFade) {
                li *= (float)(pos / kFade);
                ri *= (float)(pos / kFade);
            }
        }

        outBuf[i * 2] += li * gain;
        outBuf[i * 2 + 1] += ri * gain;

        // 切换延迟测量: 当前槽首次输出非静音样本时计算 (触发 → 出声)
        if (isCurrent && mTriggerUs.load() >= 0 &&
            (li > 0.0001f || li < -0.0001f || ri > 0.0001f || ri < -0.0001f)) {
            auto now = std::chrono::steady_clock::now().time_since_epoch();
            int64_t nowUs = std::chrono::duration_cast<std::chrono::microseconds>(now).count();
            int64_t trigger = mTriggerUs.exchange(-1);
            if (trigger >= 0) {
                mLatencyMs.store((nowUs - trigger + 500) / 1000);
                __android_log_print(ANDROID_LOG_INFO, "DrumLoop",
                                    "switch latency: %lld ms", (long long)((nowUs - trigger + 500) / 1000));
            }
        }

        pos += step;
    }

    slot.pos = pos;
}
