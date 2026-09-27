#include "DrumLoopEngine.h"
#include "TempoStream.h"
#include "SignalsmithStream.h"
#include <android/log.h>
#include <cstdio>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <utility>

// 每次 DSP 调用最多渲染约 170 ms 音频，缩短单次 CPU burst。
static constexpr size_t kRenderCapFrames = 8192;
// cut point 切换时必须已经连续准备好的目标音频。120 ms 可覆盖数次普通 Oboe 回调，
// 同时不会要求 Signalsmith 在 250 ms 提前量内渲染过大的首段。
static constexpr double kTempoCutWatermarkSec = 0.120;
// 循环边界渐变帧数
static constexpr double kFade = 256.0;

DrumLoopEngine::PcmBuffer::PcmBuffer(size_t requestedCapacityFrames) {
    if (requestedCapacityFrames == 0 ||
        requestedCapacityFrames > std::numeric_limits<size_t>::max() / 2) return;
    // 不做整块清零：未发布区域永远不会被音频线程读取，可避免变速开始时触碰整首音频的内存页。
    samples.reset(new (std::nothrow) int16_t[requestedCapacityFrames * 2]);
    if (samples) capacityFrames = requestedCapacityFrames;
}

static size_t bufferCapacityFrames(int sampleRate, double logicalDurationSec) {
    if (sampleRate <= 0 || logicalDurationSec <= 0.0 || !std::isfinite(logicalDurationSec)) return 0;
    const double logicalFrames = std::ceil(logicalDurationSec * (double)sampleRate);
    // Signalsmith 排空阶段可能因输入延迟产生少量额外帧；两块余量避免尾部截断，
    // publishedFrames 仍只会暴露真正写完的帧。
    const double capacity = logicalFrames + (double)kRenderCapFrames * 2.0;
    if (capacity <= 0.0 ||
        capacity > (double)(std::numeric_limits<size_t>::max() / 2)) return 0;
    return (size_t)capacity;
}

static int64_t publishRenderedFrames(
        const std::string& name,
        const char* engine,
        const std::shared_ptr<DrumLoopEngine::PcmBuffer>& dst,
        const int16_t* src,
        size_t frames) {
    if (!dst || !dst->valid() || !src || frames == 0) {
        return dst ? (int64_t)dst->publishedFrames.load(std::memory_order_acquire) : 0;
    }
    // 每个构建缓冲只有一个生产者，因此写指针可用 relaxed 读取；release 发布保证
    // 音频线程 acquire 读到新水位时，相应 PCM 已经全部可见。
    const size_t writeAt = dst->publishedFrames.load(std::memory_order_relaxed);
    const size_t available = writeAt < dst->capacityFrames ? dst->capacityFrames - writeAt : 0;
    const size_t copyFrames = std::min(frames, available);
    if (copyFrames > 0) {
        std::memcpy(dst->samples.get() + writeAt * 2, src, copyFrames * 2 * sizeof(int16_t));
        dst->publishedFrames.store(writeAt + copyFrames, std::memory_order_release);
    }
    if (copyFrames < frames && !dst->overflowLogged.exchange(true)) {
        __android_log_print(ANDROID_LOG_WARN, "DrumLoop",
                            "%s buffer full %s capacity=%zu dropped=%zu",
                            engine, name.c_str(), dst->capacityFrames, frames - copyFrames);
    }
    return (int64_t)(writeAt + copyFrames);
}

// 诊断: 输出缓冲前 0.8s 的 0.1s 窗口 RMS (检测渲染数据中的静音/缺陷)
static void logRmsProfile(const std::string& name, const int16_t* buf,
                          size_t total, int sampleRate) {
    if (sampleRate <= 0 || !buf || total < 2) return;
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

// ================= 高质量 (sbsms) =================

bool DrumLoopEngine::renderStart(const std::string& name, int sampleRate, float ratio, int gen,
                                 double logicalOriginSec, double logicalDurationSec) {
    if (name.empty()) return false;
    const int safeSampleRate = sampleRate > 0 ? sampleRate : 48000;
    // DSP 构造与旧对象析构都可能分配/释放较多内存，不能占用音频回调共用的 mLock。
    auto nextBuf = std::make_shared<PcmBuffer>(
        bufferCapacityFrames(safeSampleRate, logicalDurationSec));
    if (!nextBuf->valid()) return false;
    auto nextStream = std::make_shared<TempoStream>(safeSampleRate, ratio);
    std::shared_ptr<PcmBuffer> retiredPendingBuf;
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
        // SBSMS 始终写入独立构建缓冲。Signalsmith 主缓冲和已经发布的 HQ 快照
        // 都不会因下一阶段（例如“末小节”）开始渲染而被覆盖。
        retiredPendingBuf = std::move(loop->pendingBuf);
        retiredPendingStream = std::move(loop->pendingStream);
        loop->pendingBuf = std::move(nextBuf);
        loop->pendingStream = std::move(nextStream);
        loop->pendingComplete = false;
        loop->pendingOriginFrame = std::max(0.0, logicalOriginSec) * (double)safeSampleRate;
        loop->pendingLogicalFrames = logicalDurationSec > 0.0
            ? logicalDurationSec * (double)safeSampleRate : 0.0;
    }
    return true;
}

int64_t DrumLoopEngine::renderFeed(const std::string& name, const int16_t* pcm, int32_t totalSamples, int gen) {
    if (!pcm || totalSamples <= 0) return 0;
    std::shared_ptr<TempoStream> st;
    std::shared_ptr<PcmBuffer> dst;
    {
        std::lock_guard<std::mutex> lk(mLock);
        auto it = mLoops.find(name);
        if (it == mLoops.end()) return 0;
        auto loop = it->second;
        if (loop->renderGen != gen) return -1;
        st = loop->pendingStream;
        dst = loop->pendingBuf;
        if (!st || !dst) return 0;
    }
    st->feed(pcm, (size_t)totalSamples / 2);
    std::vector<int16_t> tmp(kRenderCapFrames * 2);
    size_t n = st->render(tmp.data(), kRenderCapFrames);
    if (n > 0) return publishRenderedFrames(name, "sbsms", dst, tmp.data(), n);
    return (int64_t)dst->publishedFrames.load(std::memory_order_acquire);
}

int64_t DrumLoopEngine::renderFinishStep(const std::string& name, int gen) {
    std::shared_ptr<TempoStream> st;
    std::shared_ptr<PcmBuffer> dst;
    bool drainDone = false;
    bool startDrain = false;
    int finishedSampleRate = 0;
    {
        std::lock_guard<std::mutex> lk(mLock);
        auto it = mLoops.find(name);
        if (it == mLoops.end()) return -1;
        auto loop = it->second;
        if (loop->renderGen != gen) return -1;
        st = loop->pendingStream;
        dst = loop->pendingBuf;
        if (!st || !dst) return 0;  // 已排空完成
        if (!loop->sbsmDrainStarted) {
            loop->sbsmDrainStarted = true;
            startDrain = true;
        }
    }
    if (startDrain) st->finishInput();
    // 渲染一步 (锁外)
    std::vector<int16_t> tmp(kRenderCapFrames * 2);
    size_t n = st->render(tmp.data(), kRenderCapFrames);
    if (n > 0) publishRenderedFrames(name, "sbsms", dst, tmp.data(), n);
    {
        std::lock_guard<std::mutex> lk(mLock);
        auto it = mLoops.find(name);
        if (it == mLoops.end() || it->second->renderGen != gen) return -1;
        auto loop = it->second;
        if (n == 0) {
            drainDone = true;
            loop->pendingStream.reset();
            loop->pendingComplete = true;
            __android_log_print(ANDROID_LOG_INFO, "DrumLoop",
                                "sbsms build drained %s frames=%zu",
                                name.c_str(),
                                dst->publishedFrames.load(std::memory_order_acquire));
            finishedSampleRate = loop->sampleRate;
        }
    }
    if (drainDone) {
        logRmsProfile(name, dst->samples.get(),
                      dst->publishedFrames.load(std::memory_order_acquire),
                      finishedSampleRate);
    }
    return drainDone ? 0 : (int64_t)n;
}

bool DrumLoopEngine::renderCommit(const std::string& name, int gen, int state) {
    if (state < 1 || state > 3) return false;
    std::lock_guard<std::mutex> lk(mLock);
    auto it = mLoops.find(name);
    if (it == mLoops.end()) return false;
    auto loop = it->second;
    if (loop->renderGen != gen || !loop->pendingBuf ||
        loop->pendingBuf->publishedFrames.load(std::memory_order_acquire) == 0) return false;

    loop->hqBuf = loop->pendingBuf;
    loop->hqGen = gen;
    loop->hqState = state;
    loop->hqComplete = state == 3 && loop->pendingComplete;
    loop->hqOriginFrame = loop->pendingOriginFrame;
    loop->hqLogicalFrames = loop->pendingLogicalFrames;
    if (loop->hqComplete) {
        for (auto& slot : mSlots) {
            if (slot.loop == loop && slot.buf == loop->hqBuf && slot.bufGen == gen) {
                slot.complete = true;
                slot.hqState = 3;
            }
        }
    }
    __android_log_print(ANDROID_LOG_INFO, "DrumLoop",
                        "sbsms commit %s state=%d origin=%.3f frames=%zu complete=%d",
                        name.c_str(), state,
                        loop->hqOriginFrame / (double)std::max(1, loop->sampleRate),
                        loop->hqBuf->publishedFrames.load(std::memory_order_acquire),
                        loop->hqComplete ? 1 : 0);
    return true;
}

bool DrumLoopEngine::promotePending(const std::string& name) {
    std::lock_guard<std::mutex> lk(mLock);
    auto it = mLoops.find(name);
    if (it == mLoops.end()) return false;
    auto loop = it->second;
    if (!loop->pendingBuf ||
        loop->pendingBuf->publishedFrames.load(std::memory_order_acquire) == 0) return false;
    // 兼容旧 JNI：只发布状态1快照，不再替换 Signalsmith 主缓冲，也不在循环边界
    // 自动升级。新的 Kotlin 调度器会显式调用 renderCommit() 发布四级状态。
    loop->hqBuf = loop->pendingBuf;
    loop->hqGen = loop->renderGen;
    loop->hqState = std::max(loop->hqState, 1);
    loop->hqComplete = false;
    loop->hqOriginFrame = loop->pendingOriginFrame;
    loop->hqLogicalFrames = loop->pendingLogicalFrames;
    return true;
}

// ================= 低质量低延迟 (Signalsmith) =================

bool DrumLoopEngine::sgsmStart(const std::string& name, int sampleRate, float ratio, int gen,
                               double logicalOriginSec, double logicalDurationSec) {
    if (name.empty()) return false;
    const int safeSampleRate = sampleRate > 0 ? sampleRate : 48000;
    auto nextSgsm = std::make_shared<SignalsmithStream>(safeSampleRate, ratio);
    auto nextBuf = std::make_shared<PcmBuffer>(
        bufferCapacityFrames(safeSampleRate, logicalDurationSec));
    if (!nextBuf->valid()) return false;
    auto nextBuild = std::make_shared<SgsmBuild>();
    nextBuild->stream = nextSgsm;
    nextBuild->buf = nextBuf;
    std::shared_ptr<PcmBuffer> retiredBuf;
    std::vector<std::shared_ptr<SgsmBuild>> retiredBuilds;
    {
        std::lock_guard<std::mutex> lk(mLock);
        auto loop = findOrCreateLocked(mLoops, name);
        loop->sampleRate = safeSampleRate;
        loop->ratio = ratio;
        loop->sgsmGen = gen;
        retiredBuf = std::move(loop->buf);
        // 只保留仍被某个播放槽引用的旧 generation；其 Kotlin 任务可继续向同一缓冲追加。
        for (auto it = loop->sgsmBuilds.begin(); it != loop->sgsmBuilds.end();) {
            const auto& old = it->second;
            const bool active = old &&
                ((mSlots[0].loop == loop && mSlots[0].buf == old->buf) ||
                 (mSlots[1].loop == loop && mSlots[1].buf == old->buf));
            if (!active) {
                retiredBuilds.emplace_back(std::move(it->second));
                it = loop->sgsmBuilds.erase(it);
            } else {
                ++it;
            }
        }
        loop->sgsmBuilds[gen] = nextBuild;
        loop->sgsm = nextSgsm;
        loop->buf = nextBuf;
        loop->bufGen = gen;
        loop->bufOriginFrame = std::max(0.0, logicalOriginSec) * (double)safeSampleRate;
        loop->bufLogicalFrames = logicalDurationSec > 0.0
            ? logicalDurationSec * (double)safeSampleRate : 0.0;
        loop->complete = false;
        loop->sgsmDrainStarted = false;
        loop->promoteAtWrap = false;
    }
    return true;
}

int64_t DrumLoopEngine::sgsmFeed(const std::string& name, const int16_t* pcm, int32_t totalSamples, int gen) {
    if (!pcm || totalSamples <= 0) return 0;
    std::shared_ptr<SgsmBuild> build;
    {
        std::lock_guard<std::mutex> lk(mLock);
        auto it = mLoops.find(name);
        if (it == mLoops.end()) return 0;
        auto loop = it->second;
        auto buildIt = loop->sgsmBuilds.find(gen);
        if (buildIt == loop->sgsmBuilds.end()) return -1;
        build = buildIt->second;
        if (!build || !build->stream || !build->buf) return 0;
    }
    build->stream->feed(pcm, (size_t)totalSamples / 2);
    std::vector<int16_t> tmp(kRenderCapFrames * 2);
    size_t n = build->stream->render(tmp.data(), kRenderCapFrames);
    if (n > 0) return publishRenderedFrames(name, "signalsmith", build->buf, tmp.data(), n);
    return (int64_t)build->buf->publishedFrames.load(std::memory_order_acquire);
}

int64_t DrumLoopEngine::sgsmFinishStep(const std::string& name, int gen) {
    std::shared_ptr<SgsmBuild> build;
    bool drainDone = false;
    bool startDrain = false;
    int finishedSampleRate = 0;
    {
        std::lock_guard<std::mutex> lk(mLock);
        auto it = mLoops.find(name);
        if (it == mLoops.end()) return -1;
        auto loop = it->second;
        auto buildIt = loop->sgsmBuilds.find(gen);
        if (buildIt == loop->sgsmBuilds.end()) return -1;
        build = buildIt->second;
        if (!build || !build->stream || !build->buf) return 0;
        if (!build->drainStarted) {
            build->drainStarted = true;
            startDrain = true;
        }
    }
    if (startDrain) build->stream->finishInput();
    std::vector<int16_t> tmp(kRenderCapFrames * 2);
    size_t n = build->stream->render(tmp.data(), kRenderCapFrames);
    if (n > 0) publishRenderedFrames(name, "signalsmith", build->buf, tmp.data(), n);
    {
        std::lock_guard<std::mutex> lk(mLock);
        auto it = mLoops.find(name);
        if (it == mLoops.end()) return -1;
        auto loop = it->second;
        auto buildIt = loop->sgsmBuilds.find(gen);
        if (buildIt == loop->sgsmBuilds.end() || buildIt->second != build) return -1;
        if (n == 0) {
            drainDone = true;
            build->complete = true;
            for (auto& slot : mSlots) {
                if (slot.loop == loop && slot.buf == build->buf && slot.bufGen == gen) {
                    slot.complete = true;
                }
            }
            if (loop->bufGen == gen && loop->buf == build->buf) {
                loop->complete = true;
                loop->sgsm.reset();
            }
            __android_log_print(ANDROID_LOG_INFO, "DrumLoop",
                                "sgsm drained %s frames=%zu",
                                name.c_str(),
                                build->buf->publishedFrames.load(std::memory_order_acquire));
            finishedSampleRate = loop->sampleRate;
            loop->sgsmBuilds.erase(buildIt);
        }
    }
    if (drainDone) {
        logRmsProfile(name, build->buf->samples.get(),
                      build->buf->publishedFrames.load(std::memory_order_acquire),
                      finishedSampleRate);
    }
    return (int64_t)n;
}

void DrumLoopEngine::renderCancelAll() {
    std::vector<std::shared_ptr<TempoStream>> retiredTempoStreams;
    std::vector<std::shared_ptr<PcmBuffer>> retiredBuffers;
    // 常见循环包远少于此数量；预分配放在锁外，避免 detach 阶段触发容器扩容。
    retiredTempoStreams.reserve(128);
    retiredBuffers.reserve(128);
    {
        std::lock_guard<std::mutex> lk(mLock);
        for (auto& kv : mLoops) {
            auto& loop = kv.second;
            if (loop->stream) retiredTempoStreams.emplace_back(std::move(loop->stream));
            if (loop->pendingStream) retiredTempoStreams.emplace_back(std::move(loop->pendingStream));
            if (loop->pendingBuf) retiredBuffers.emplace_back(std::move(loop->pendingBuf));
            if (loop->hqBuf) retiredBuffers.emplace_back(std::move(loop->hqBuf));
            loop->pendingComplete = false;
            loop->pendingPlayable = false;
            loop->pendingOriginFrame = 0.0;
            loop->pendingLogicalFrames = 0.0;
            loop->sbsmDrainStarted = false;
            loop->sgsmDrainStarted = false;
            loop->promoteAtWrap = false;
            loop->hqGen = -1;
            loop->hqState = 0;
            loop->hqComplete = false;
            loop->hqOriginFrame = 0.0;
            loop->hqLogicalFrames = 0.0;
            // BPM 改变: 旧 HQ 已失效；非播放循环清掉主指针。仍被槽/sgsmBuilds
            // 引用的 Signalsmith 缓冲可以继续增长，供连续变速的当前代平稳播到下个 cut。
            if (!isLoopActive(loop)) {
                if (loop->buf) retiredBuffers.emplace_back(std::move(loop->buf));
                loop->bufGen = -1;
                loop->bufOriginFrame = 0.0;
                loop->bufLogicalFrames = 0.0;
                loop->complete = false;
            }
        }
        mTempoCutArmed = false;
        mTempoCutName.clear();
        mTempoCutGen = -1;
        mTempoCutRemainingSec = 0.0;
        mTempoCutTargetBpm = -1.0;
    }
    // retired* 在 mLock 之外析构，避免 Oboe 回调等待 DSP/大 PCM 缓冲释放。
    __android_log_print(ANDROID_LOG_INFO, "DrumLoop", "renderCancelAll (invalidate HQ snapshots)");
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
        mTempoCutArmed = false;
        mTempoCutName.clear();
        mTempoCutGen = -1;
    }
    // Loop、DSP 与 PCM 的实际释放发生在播放锁之外。
}

// ================= 播放 (双槽 ping-pong) =================

void DrumLoopEngine::play(const std::string& name) {
    playAt(name, 0.0);
}

void DrumLoopEngine::playAt(const std::string& name, double logicalStartSec) {
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    int64_t nowUs = std::chrono::duration_cast<std::chrono::microseconds>(now).count();
    {
        std::lock_guard<std::mutex> lk(mLock);
        auto it = mLoops.find(name);
        if (it == mLoops.end()) return;
        auto loop = it->second;

        std::shared_ptr<PcmBuffer> selectedBuf = loop->buf;
        int selectedGen = loop->bufGen;
        double selectedOrigin = loop->bufOriginFrame;
        double selectedDuration = loop->bufLogicalFrames;
        bool selectedComplete = loop->complete;
        int selectedHqState = 0;
        double selectedPos = 0.0;

        // 状态1可从头起播；状态2额外允许从末小节任意位置起播；状态3允许任意位置。
        // 物理缓冲采用旋转布局时，通过 origin 把逻辑时间映射到正确的物理帧。
        if (loop->hqBuf && loop->hqGen >= 0 && loop->hqState > 0 &&
            loop->hqLogicalFrames > 0.0) {
            const double logicalDuration = loop->hqLogicalFrames;
            double logicalFrame = std::max(0.0, logicalStartSec) * (double)loop->sampleRate;
            logicalFrame = std::fmod(logicalFrame, logicalDuration);
            if (logicalFrame < 0.0) logicalFrame += logicalDuration;
            const bool fromStart = logicalFrame < 1.0;
            const bool inLastMeasure = loop->hqState >= 2 &&
                logicalFrame + 1.0 >= loop->hqOriginFrame;
            const bool allowed = loop->hqState >= 3 || fromStart || inLastMeasure;
            double physical = std::fmod(
                logicalFrame - loop->hqOriginFrame + logicalDuration, logicalDuration);
            if (physical < 0.0) physical += logicalDuration;
            const double availableFrames = (double)loop->hqBuf->publishedFrames.load(
                std::memory_order_acquire);
            // 线性插值在末帧会回取物理第 0 帧，因此完整/末段缓冲的最后一个
            // 可寻址帧同样是合法起点，不必人为排除最后约 1 帧。
            if (allowed && availableFrames >= 2.0 && physical < availableFrames) {
                selectedBuf = loop->hqBuf;
                selectedGen = loop->hqGen;
                selectedOrigin = loop->hqOriginFrame;
                selectedDuration = loop->hqLogicalFrames;
                selectedComplete = loop->hqComplete;
                selectedHqState = loop->hqState;
                selectedPos = physical;
            }
        }

        if (selectedHqState == 0 && selectedDuration > 0.0) {
            double logicalFrame = std::max(0.0, logicalStartSec) * (double)loop->sampleRate;
            logicalFrame = std::fmod(logicalFrame, selectedDuration);
            if (logicalFrame < 0.0) logicalFrame += selectedDuration;
            selectedPos = std::fmod(
                logicalFrame - selectedOrigin + selectedDuration, selectedDuration);
            if (selectedPos < 0.0) selectedPos += selectedDuration;
        }

        if (!selectedBuf) return;
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
        mSlots[nu].buf = selectedBuf;
        mSlots[nu].bufGen = selectedGen;
        mSlots[nu].sampleRate = loop->sampleRate;
        mSlots[nu].logicalOriginFrame = selectedOrigin;
        mSlots[nu].logicalDurationFrames = selectedDuration;
        mSlots[nu].complete = selectedComplete;
        mSlots[nu].hqState = selectedHqState;
        mSlots[nu].pos = selectedPos;
        mSlots[nu].gain = 1.0f;
        mSlots[nu].fadeInRemainingUs = kSwitchFadeInUs;
        mSlots[nu].fading = false;
        mCurSlot = nu;

        __android_log_print(ANDROID_LOG_INFO, "DrumLoop",
                            "play source %s: %s state=%d gen=%d start=%.3f physical=%.3f complete=%d",
                            name.c_str(), selectedHqState > 0 ? "sbsms" : "signalsmith",
                            selectedHqState, selectedGen, logicalStartSec,
                            selectedPos / (double)std::max(1, loop->sampleRate),
                            selectedComplete ? 1 : 0);

        // 调试: 新循环缓冲头部峰值与前 16 个样本
        if (selectedBuf) {
            const size_t published = selectedBuf->publishedFrames.load(std::memory_order_acquire);
            const int16_t* b = selectedBuf->samples.get();
            const int64_t n = std::min<int64_t>((int64_t)published, it->second->sampleRate / 10);
            int peak = 0;
            int64_t peakAt = 0;
            for (int64_t i = 0; i < n; i++) {
                int v = std::max(std::abs((int)b[i * 2]), std::abs((int)b[i * 2 + 1]));
                if (v > peak) { peak = v; peakAt = i; }
            }
            char head[256] = {0};
            int off = 0;
            for (int64_t i = 0; i < 16 && i < (int64_t)published; i++)
                off += snprintf(head + off, sizeof(head) - off, "%d ", (int)b[i * 2]);
            __android_log_print(ANDROID_LOG_INFO, "DrumLoop",
                                "play %s slot=%d frames=%lld headPeak=%d(%.2f)@%lld head=[%s]",
                                name.c_str(), nu, (long long)published, peak,
                                peak / 32768.0, (long long)peakAt, head);
        }
    }
    // 记录切换触发时刻 (混音器首次出声时计算延迟)
    mTriggerUs.store(nowUs);
    mLatencyMs.store(-1);
    mFading.store(0);
    __android_log_print(ANDROID_LOG_INFO, "DrumLoop", "play %s at %.3f", name.c_str(), logicalStartSec);
}

void DrumLoopEngine::stop() {
    std::lock_guard<std::mutex> lk(mLock);
    mSlots[0].loop.reset();
    mSlots[1].loop.reset();
    mTempoCutArmed = false;
    mTempoCutName.clear();
    mTempoCutGen = -1;
}

bool DrumLoopEngine::isPlaying() const {
    std::lock_guard<std::mutex> lk(mLock);
    return (bool)mSlots[0].loop || (bool)mSlots[1].loop;
}

double DrumLoopEngine::getPlaybackPositionSec(const std::string& name) const {
    std::lock_guard<std::mutex> lk(mLock);
    const Slot& slot = mSlots[mCurSlot];
    if (!slot.loop || slot.loop->name != name || slot.sampleRate <= 0) return -1.0;
    double logicalFrame = slot.logicalOriginFrame + slot.pos;
    if (slot.logicalDurationFrames > 0.0) {
        logicalFrame = std::fmod(logicalFrame, slot.logicalDurationFrames);
        if (logicalFrame < 0.0) logicalFrame += slot.logicalDurationFrames;
    }
    return logicalFrame / (double)slot.sampleRate;
}

bool DrumLoopEngine::armTempoCut(const std::string& name, int gen, double delaySec,
                                 const std::vector<double>& targetOffsetsSec,
                                 const std::vector<double>& nextDelaysSec,
                                 double targetBpm) {
    if (name.empty() || gen < 0 || delaySec < 0.0 || targetBpm <= 0.0 ||
        targetOffsetsSec.empty() || targetOffsetsSec.size() != nextDelaysSec.size()) return false;
    std::vector<TempoCutCandidate> candidates;
    candidates.reserve(targetOffsetsSec.size());
    for (size_t i = 0; i < targetOffsetsSec.size(); ++i) {
        if (!std::isfinite(targetOffsetsSec[i]) || targetOffsetsSec[i] < 0.0 ||
            !std::isfinite(nextDelaysSec[i]) || nextDelaysSec[i] <= 0.0) return false;
        candidates.push_back({targetOffsetsSec[i], nextDelaysSec[i]});
    }
    std::lock_guard<std::mutex> lk(mLock);
    const Slot& current = mSlots[mCurSlot];
    auto it = mLoops.find(name);
    if (!current.loop || current.loop->name != name || it == mLoops.end()) return false;
    const auto& target = it->second;
    if (target->bufGen != gen || target->sgsmGen != gen || !target->buf) return false;

    mTempoCutArmed = true;
    mTempoCutName = name;
    mTempoCutGen = gen;
    mTempoCutRemainingSec = delaySec;
    mTempoCutTargetBpm = targetBpm;
    mTempoCutCandidates = std::move(candidates);
    mTempoCutCandidateIndex = 0;
    __android_log_print(ANDROID_LOG_INFO, "DrumLoop",
                        "tempo cut armed %s gen=%d delay=%.3f bpm=%.1f origin=%.3f cuts=%zu",
                        name.c_str(), gen, delaySec, targetBpm,
                        target->bufOriginFrame / (double)std::max(1, target->sampleRate),
                        mTempoCutCandidates.size());
    return true;
}

void DrumLoopEngine::cancelTempoCut() {
    std::lock_guard<std::mutex> lk(mLock);
    mTempoCutArmed = false;
    mTempoCutName.clear();
    mTempoCutGen = -1;
    mTempoCutRemainingSec = 0.0;
    mTempoCutTargetBpm = -1.0;
    mTempoCutCandidateIndex = 0;
}

double DrumLoopEngine::getAndClearTempoCutEvent() {
    return mTempoCutEvent.exchange(-1.0);
}

double DrumLoopEngine::fireTempoCutLocked() {
    if (!mTempoCutArmed) return -1.0;
    auto it = mLoops.find(mTempoCutName);
    Slot& current = mSlots[mCurSlot];
    if (!current.loop || current.loop->name != mTempoCutName ||
        it == mLoops.end() || it->second->bufGen != mTempoCutGen ||
        mTempoCutCandidates.empty()) {
        mTempoCutArmed = false;
        return -1.0;
    }
    auto loop = it->second;
    if (!loop->buf || !loop->buf->valid()) {
        mTempoCutArmed = false;
        return -1.0;
    }

    const TempoCutCandidate& candidate =
        mTempoCutCandidates[mTempoCutCandidateIndex % mTempoCutCandidates.size()];
    const size_t published = loop->buf->publishedFrames.load(std::memory_order_acquire);
    const size_t startFrame = (size_t)std::llround(
        candidate.targetOffsetSec * (double)std::max(1, loop->sampleRate));
    const size_t watermark = (size_t)std::ceil(
        kTempoCutWatermarkSec * (double)std::max(1, loop->sampleRate));
    const bool enough = startFrame < published &&
        (loop->complete || published - startFrame >= watermark + 2);
    if (!enough) {
        // 本切分点准备不足：旧槽继续播放，不在切分点之后追赶切换；直接安排下一个
        // JSON cut point。整个动作只更新固定计划中的索引和倒计时，不做分配。
        const size_t missedIndex = mTempoCutCandidateIndex;
        mTempoCutRemainingSec = candidate.nextDelaySec;
        mTempoCutCandidateIndex = (mTempoCutCandidateIndex + 1) % mTempoCutCandidates.size();
        __android_log_print(ANDROID_LOG_WARN, "DrumLoop",
                            "tempo cut skipped %s gen=%d cut=%zu ready=%zu needFrom=%zu next=%.3f",
                            mTempoCutName.c_str(), mTempoCutGen, missedIndex,
                            published, startFrame + watermark + 2, mTempoCutRemainingSec);
        return -1.0;
    }

    current.loop = loop;
    current.buf = loop->buf;
    current.bufGen = loop->bufGen;
    current.sampleRate = loop->sampleRate;
    current.logicalOriginFrame = loop->bufOriginFrame;
    current.logicalDurationFrames = loop->bufLogicalFrames;
    current.complete = loop->complete;
    current.hqState = 0;
    current.pos = (double)startFrame;
    current.gain = 1.0f;
    current.fading = false;
    current.fadeInRemainingUs = 0; // cut point 已选在低能量位置，不引入额外起播延迟

    const double appliedBpm = mTempoCutTargetBpm;
    mTempoCutArmed = false;
    mTempoCutName.clear();
    mTempoCutGen = -1;
    mTempoCutRemainingSec = 0.0;
    mTempoCutTargetBpm = -1.0;
    __android_log_print(ANDROID_LOG_INFO, "DrumLoop",
                        "tempo cut fired bpm=%.1f origin=%.3f buffered=%zu",
                        appliedBpm,
                        current.logicalOriginFrame / (double)std::max(1, current.sampleRate),
                        published);
    return appliedBpm;
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
    double appliedBpm = -1.0;
    TempoCutCallback callback = nullptr;
    {
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
                mTempoCutArmed = false;
                __android_log_print(ANDROID_LOG_INFO, "DrumLoop", "fadeOut complete");
                return;
            }
            masterFade = (float)((double)mFadeRemainingUs / (double)mFadeTotalUs);
        }
        if (vol <= 0.0f) return;

        const auto mixSegment = [&](float* dst, int32_t frames) {
            if (frames <= 0) return;
            mixSlot(mSlots[1 - mCurSlot], false, dst, frames,
                    deviceSampleRate, vol * masterFade);
            mixSlot(mSlots[mCurSlot], true, dst, frames,
                    deviceSampleRate, vol * masterFade);
        };

        int32_t beforeCut = numFrames;
        if (mTempoCutArmed) {
            const double exactFrames = std::max(0.0, mTempoCutRemainingSec) *
                                       (double)deviceSampleRate;
            beforeCut = (int32_t)std::min<double>((double)numFrames, std::ceil(exactFrames));
        }

        mixSegment(outBuf, beforeCut);
        if (mTempoCutArmed) {
            mTempoCutRemainingSec = std::max(
                0.0, mTempoCutRemainingSec - (double)beforeCut / (double)deviceSampleRate);
            if (mTempoCutRemainingSec <= 0.0) {
                appliedBpm = fireTempoCutLocked();
                if (appliedBpm > 0.0) callback = mTempoCutCallback;
            }
        }
        const int32_t afterCut = numFrames - beforeCut;
        mixSegment(outBuf + (size_t)beforeCut * 2, afterCut);
    }

    // 回调放在播放锁之外；只发布原子状态与更新 BeatTracker，不参与 DSP/分配。
    if (appliedBpm > 0.0) {
        mTempoCutEvent.store(appliedBpm);
        if (callback) callback(appliedBpm);
    }
}

void DrumLoopEngine::mixSlot(Slot& slot, bool isCurrent, float* outBuf, int32_t numFrames,
                             int32_t deviceSampleRate, float baseVol) {
    if (!slot.loop) return;
    // 播放缓冲快照: 未设置则取 loop 当前缓冲
    if (!slot.buf) slot.buf = slot.loop->buf;
    if (!slot.buf || !slot.buf->valid()) return;
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

    const int16_t* cur = slot.buf->samples.get();
    int64_t totalFrames = (int64_t)slot.buf->publishedFrames.load(std::memory_order_acquire);
    if (totalFrames < 2) return;

    // 普通状态0点击播放仍允许低延迟启动；变速切换另由 fireTempoCutLocked 的
    // 120 ms 水位把关，不会依赖这里的起播等待。
    if (isCurrent && !slot.complete) {
        const int64_t watermark = (int64_t)loop->sampleRate * 2 / 100;
        if (totalFrames < watermark) return;
    }

    double pos = slot.pos;
    const double step = (double)loop->sampleRate / (double)deviceSampleRate * (double)mRate.load();
    const float kScale = 1.0f / 32768.0f;

    for (int32_t i = 0; i < numFrames; i++) {
        if (pos >= (double)totalFrames) {
            if (!isCurrent) break;   // 淡出中的旧槽: 播完即止
            // 头部快照播到末端时，如果更高状态已经发布，或同一状态已重新发布为
            // 仍在增长的新缓冲，则按逻辑时间重绑后继续。后者用于状态1的 MediaCodec
            // 停放/恢复，避免播放槽永远抓住旧的固定0.5秒快照。
            // Signalsmith(hqState=0) 仍留给后续独立的高质量 cut-point 切换功能处理。
            bool upgraded = false;
            const bool newerHqState = loop->hqState > slot.hqState;
            const bool refreshedSameState = loop->hqState == slot.hqState &&
                                            loop->hqBuf && loop->hqBuf != slot.buf;
            if (slot.hqState > 0 && loop->hqBuf && loop->hqGen == slot.bufGen &&
                (newerHqState || refreshedSameState) && loop->hqLogicalFrames > 0.0) {
                double logicalFrame = slot.logicalOriginFrame + pos;
                logicalFrame = std::fmod(logicalFrame, loop->hqLogicalFrames);
                if (logicalFrame < 0.0) logicalFrame += loop->hqLogicalFrames;
                double physical = std::fmod(
                    logicalFrame - loop->hqOriginFrame + loop->hqLogicalFrames,
                    loop->hqLogicalFrames);
                if (physical < 0.0) physical += loop->hqLogicalFrames;
                const int64_t candidateFrames = (int64_t)loop->hqBuf->publishedFrames.load(
                    std::memory_order_acquire);
                if (candidateFrames >= 2 && physical < (double)candidateFrames) {
                    slot.buf = loop->hqBuf;
                    slot.bufGen = loop->hqGen;
                    slot.logicalOriginFrame = loop->hqOriginFrame;
                    slot.logicalDurationFrames = loop->hqLogicalFrames;
                    slot.complete = loop->hqComplete;
                    slot.hqState = loop->hqState;
                    cur = slot.buf->samples.get();
                    totalFrames = candidateFrames;
                    pos = physical;
                    upgraded = true;
                }
            }
            if (!upgraded && !slot.complete) {
                static int underrunLogSkip = 0;
                if ((underrunLogSkip++ % 100) == 0) {
                    __android_log_print(ANDROID_LOG_WARN, "DrumLoop",
                                        "underrun hold: %s buf=%lld pos=%.0f",
                                        loop->name.c_str(), (long long)totalFrames, pos);
                }
                pos = (double)totalFrames;
                break;
            } else if (!upgraded) {
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
        float li = (cur[b0] * kScale) + ((cur[b1] * kScale) - (cur[b0] * kScale)) * (float)frac;
        float ri = (cur[b0 + 1] * kScale) + ((cur[b1 + 1] * kScale) - (cur[b0 + 1] * kScale)) * (float)frac;

        // 回卷处渐变 (仅完整缓冲)
        if (isCurrent && slot.complete) {
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
