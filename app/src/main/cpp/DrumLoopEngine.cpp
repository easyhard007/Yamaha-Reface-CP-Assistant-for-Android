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
static constexpr int64_t kBassNoChordHoldUs = 5000000;
// 补弹使用前陡后缓的 stretched exponential：一个三十二分音符时约 65%，
// 一个八分音符时仍约 40%，比旧曲线更快压低刚开始处的重复音头。
static constexpr double kBassCatchUpAtThirtySecond = 0.65;
static constexpr double kBassCatchUpCurveExponent = 0.545;
static constexpr double kBassHighVelocityScale = 0.60;
// Root/bass notes use A1..G#2 (33..44). Connected C5 accents are exactly one
// or two octaves higher so adjacent authored C5 notes can use close voice leading.
static constexpr int kBassLowRegisterStart = 33;
static constexpr int kBassHighRegisterStart = 45;
static constexpr int kBassHighRegisterEnd = 68;

static int normalizeBassPc(int pitch) {
    int pc = pitch % 12;
    if (pc < 0) pc += 12;
    return pc;
}

static int pitchClassInARegister(int pc, int aPitch) {
    pc = normalizeBassPc(pc);
    return pc >= 9 ? aPitch + (pc - 9) : aPitch + 3 + pc;
}

static bool containsBassPc(const std::vector<int>& pitchClasses, int wantedPc) {
    wantedPc = normalizeBassPc(wantedPc);
    return std::any_of(pitchClasses.begin(), pitchClasses.end(),
                       [wantedPc](int pitch) {
                           return normalizeBassPc(pitch) == wantedPc;
                       });
}

static int64_t steadyNowMicros() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::microseconds>(now).count();
}

DrumLoopEngine::DrumLoopEngine() {
    mBassChordTimerThread = std::thread(&DrumLoopEngine::bassChordTimerLoop, this);
}

DrumLoopEngine::~DrumLoopEngine() {
    {
        std::lock_guard<std::mutex> lk(mLock);
        mBassChordTimerStopping = true;
    }
    mBassChordTimerCv.notify_one();
    if (mBassChordTimerThread.joinable()) mBassChordTimerThread.join();
}

// 30 阶 constant-power sin 曲线；A 槽反向索引即 cos。查表避免实时音频线程逐帧
// 调用三角函数。
static constexpr float kConstantPowerSin[31] = {
    0.000000000f, 0.052335956f, 0.104528463f, 0.156434465f,
    0.207911691f, 0.258819045f, 0.309016994f, 0.358367950f,
    0.406736643f, 0.453990500f, 0.500000000f, 0.544639035f,
    0.587785252f, 0.629320391f, 0.669130606f, 0.707106781f,
    0.743144825f, 0.777145961f, 0.809016994f, 0.838670568f,
    0.866025404f, 0.891006524f, 0.913545458f, 0.933580426f,
    0.951056516f, 0.965925826f, 0.978147601f, 0.987688341f,
    0.994521895f, 0.998629535f, 1.000000000f
};

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

bool DrumLoopEngine::setBassMidiEvents(const std::string& name,
                                       const std::vector<double>& phases,
                                       const std::vector<int>& velocities,
                                       const std::vector<int>& downbeats,
                                       const std::vector<int>& strongBeats,
                                       const std::vector<int>& noteKinds,
                                       const std::vector<int>& phraseGroups) {
    if (name.empty() || phases.size() != velocities.size() ||
        phases.size() != downbeats.size() || phases.size() != strongBeats.size() ||
        phases.size() != noteKinds.size() || phases.size() != phraseGroups.size()) return false;
    std::vector<BassMidiEvent> events;
    events.reserve(phases.size());
    for (size_t i = 0; i < phases.size(); ++i) {
        if (!std::isfinite(phases[i]) || phases[i] < 0.0 || phases[i] > 1.0 ||
            velocities[i] < 0 || velocities[i] > 127) return false;
        if (downbeats[i] != 0 && downbeats[i] != 1) return false;
        if (strongBeats[i] != 0 && strongBeats[i] != 1) return false;
        if (noteKinds[i] < 0 || noteKinds[i] > 2 || phraseGroups[i] < -1) return false;
        if (noteKinds[i] == 2 && phraseGroups[i] < 0) return false;
        events.push_back({phases[i], velocities[i], downbeats[i] != 0,
                          strongBeats[i] != 0, noteKinds[i], phraseGroups[i]});
    }
    std::stable_sort(events.begin(), events.end(),
                     [](const BassMidiEvent& a, const BassMidiEvent& b) {
                         return a.phase < b.phase;
                     });
    std::lock_guard<std::mutex> lk(mLock);
    auto loop = findOrCreateLocked(mLoops, name);
    // BREAK 是刻意留白的鼓段：即使资源目录提供了同名 MIDI，也不允许
    // 其中任何事件进入贝斯 transport。保留注册调用以便覆盖旧缓存。
    if (name == "BREAK") {
        loop->bassMidi.clear();
        __android_log_print(ANDROID_LOG_INFO, "DrumLoop",
                            "ignore bass MIDI for BREAK (%zu events)", events.size());
    } else {
        loop->bassMidi = std::move(events);
    }
    if (mSlots[mCurSlot].loop == loop) mBassMidiNeedsSync = true;
    return true;
}

void DrumLoopEngine::setBassChordContext(int fallbackPitch, int rootPitch,
                                         const std::vector<int>& pitchClasses,
                                         double tempoBpm) {
    std::vector<int> pitches;
    std::vector<int> tonePcs;
    if (fallbackPitch >= 0 && fallbackPitch <= 127) pitches.push_back(fallbackPitch);
    for (int pitchClass : pitchClasses) {
        int pc = pitchClass % 12;
        if (pc < 0) pc += 12;
        if (std::find(tonePcs.begin(), tonePcs.end(), pc) == tonePcs.end()) {
            tonePcs.push_back(pc);
        }
        // Same compact A1..G#2 register used by BassAssist.
        const int pitch = pitchClassInARegister(pc, kBassLowRegisterStart);
        if (std::find(pitches.begin(), pitches.end(), pitch) == pitches.end()) {
            pitches.push_back(pitch);
        }
    }
    const int rootPc = rootPitch >= 0 ? normalizeBassPc(rootPitch)
                                      : (fallbackPitch >= 0
                                             ? normalizeBassPc(fallbackPitch) : -1);
    if (rootPc >= 0 &&
        std::find(tonePcs.begin(), tonePcs.end(), rootPc) == tonePcs.end()) {
        tonePcs.insert(tonePcs.begin(), rootPc);
    }
    std::unique_lock<std::mutex> lk(mLock);
    if (std::isfinite(tempoBpm) && tempoBpm >= 30.0 && tempoBpm <= 300.0) {
        mBassTempoBpm = tempoBpm;
    }

    // ChordDetector 的“无和弦”只是全局和弦暂时为空。贝斯继续沿用上一个
    // 有效和弦；只有 5 秒内始终没有新和弦时，专用原生计时线程才会清空它。
    if (fallbackPitch < 0 || fallbackPitch > 127) {
        mGlobalBassChordPresent = false;
        mBassNoChordDeadlineUs = steadyNowMicros() + kBassNoChordHoldUs;
        lk.unlock();
        mBassChordTimerCv.notify_one();
        __android_log_print(ANDROID_LOG_INFO, "DrumLoop",
                            "global chord empty; retain bass chord for 5s");
        return;
    }

    mGlobalBassChordPresent = true;
    mBassNoChordDeadlineUs = -1;
    mBassChordTimerCv.notify_one();
    const int previousFallback = mChordBassPitch.load(std::memory_order_relaxed);
    const bool sameLowCandidates = mBassChordPitches == pitches;
    const bool sameToneCandidates = mBassChordTonePcs == tonePcs;
    const int previousRootPc = mBassChordRootPc;
    const int previousThirdPc = mBassChordThirdPc;
    const int previousFifthPc = mBassChordFifthPc;
    mChordBassPitch.store(fallbackPitch, std::memory_order_relaxed);
    mBassChordPitches = std::move(pitches);
    mBassChordTonePcs = std::move(tonePcs);
    mBassChordRootPc = rootPc;
    mBassChordThirdPc = -1;
    mBassChordFifthPc = -1;
    if (rootPc >= 0) {
        // Prefer literal chord thirds/fifths. Diminished/augmented fifths and
        // suspended seconds/fourths are musical fallbacks when a conventional
        // third or fifth is absent.
        for (int interval : {7, 6, 8}) {
            const int pc = (rootPc + interval) % 12;
            if (containsBassPc(pitchClasses, pc)) {
                mBassChordFifthPc = pc;
                break;
            }
        }
        // ChordDetector keeps template order, so the structural third precedes
        // extensions such as #9. Preserve that order instead of blindly preferring
        // a minor third whenever both pitch classes are present.
        for (int pitchClass : pitchClasses) {
            const int pc = normalizeBassPc(pitchClass);
            const int interval = (pc - rootPc + 12) % 12;
            if (interval == 3 || interval == 4 || interval == 2 || interval == 5) {
                mBassChordThirdPc = pc;
                break;
            }
        }
        if (mBassChordFifthPc < 0) mBassChordFifthPc = rootPc;
        if (mBassChordThirdPc < 0) mBassChordThirdPc = rootPc;
    }

    // BassAssist's second scenario: a chord can arrive just after the rhythmic
    // trigger, or change while the note is still gated. Keep the MIDI gate and
    // its original note-off, but immediately replace/catch up the sounding note
    // with a velocity that decays from the original MIDI note-on.
    if (!mBassLoopEnabled.load(std::memory_order_relaxed) ||
        !mBassMidiInputOn || !mBassMidiCallback) return;
    if (previousFallback == fallbackPitch && sameLowCandidates && sameToneCandidates &&
        previousRootPc == mBassChordRootPc && previousThirdPc == mBassChordThirdPc &&
        previousFifthPc == mBassChordFifthPc && mBassMidiOutputPitch >= 0) return;

    // Am -> Am7 等同根变化只扩展后续选音，不打断当前音。即使当前 C4
    // 位于第 1/3 拍附近，同根仍拥有最高优先级。
    if (previousRootPc >= 0 && previousRootPc == mBassChordRootPc) return;

    // C5 是装饰性高音：当前音必须按原 MIDI 时值自然结束，和弦变化只影响
    // 后续 C5 的选音，绝不在中途 NoteOff/NoteOn。
    if (mBassMidiInputKind != 0) return;

    // C4 必须还会持续超过半拍才值得补弹。第 1/3 拍附近（误差不超过
    // 三十二分音符）的起音是重音锚点：即便旧音碰巧是新和弦三/五音也
    // 要换成新根音；若旧音已经是新根音，则没有必要制造重复音头。
    if (bassGateRemainingBeatsLocked() <= 0.5) return;
    if (mBassMidiOutputPitch >= 0) {
        const int outputPc = normalizeBassPc(mBassMidiOutputPitch);
        if (outputPc == mBassChordRootPc) return;
        const bool isThirdOrFifth = outputPc == mBassChordThirdPc ||
                                    outputPc == mBassChordFifthPc;
        if (isThirdOrFifth && !mBassMidiInputStrongBeat) return;
    }

    const int catchUpVelocity = bassCatchUpVelocityLocked();
    if (mBassMidiOutputPitch >= 0) {
        mBassMidiCallback(BassNoteOff, mBassMidiOutputPitch, 0);
        mBassMidiOutputPitch = -1;
    }
    if (catchUpVelocity > 0) {
        const int pitch = fallbackPitch;
        if (pitch >= 0) {
            mBassMidiCallback(BassNoteOn, pitch, catchUpVelocity);
            mBassMidiOutputPitch = pitch;
        }
    }
}

void DrumLoopEngine::setBassSelectionEnergy(float energy) {
    if (!std::isfinite(energy)) energy = 0.0f;
    mBassSelectionEnergy.store(std::max(0.0f, std::min(1.0f, energy)),
                               std::memory_order_relaxed);
}

void DrumLoopEngine::setBassLoopEnabled(bool enabled) {
    const bool changed = mBassLoopEnabled.exchange(enabled) != enabled;
    if (!changed) return;
    std::lock_guard<std::mutex> lk(mLock);
    resetBassMidiLocked(true);
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
        mAutoFillJumpArmed = false;
        mAutoFillJumpName.clear();
        mAutoFillJumpGen = -1;
        mAutoFillJumpRemainingSec = 0.0;
        mAutoFillJumpDestinationSec = 0.0;
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
        mAutoFillJumpArmed = false;
        mAutoFillJumpName.clear();
        mAutoFillJumpGen = -1;
        clearOneShotLocked();
        mOneShotStartedEvent.store(0);
        mOneShotStartedEventName.clear();
        mOneShotSwitchEvent.store(0);
        mOneShotSwitchEventName.clear();
        resetBassMidiLocked(true);
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
        mSlots[nu].constantPowerRole = 0;
        mSlots[nu].constantPowerTotalFrames = 0;
        mSlots[nu].constantPowerProgressFrames = 0;
        mSlots[old].constantPowerRole = 0;
        mSlots[old].constantPowerTotalFrames = 0;
        mSlots[old].constantPowerProgressFrames = 0;
        mCurSlot = nu;
        mAutoFillJumpArmed = false;
        mAutoFillJumpName.clear();
        mAutoFillJumpGen = -1;
        mAutoFillJumpRemainingSec = 0.0;
        mAutoFillJumpDestinationSec = 0.0;
        clearOneShotLocked();
        mOneShotStartedEvent.store(0);
        mOneShotStartedEventName.clear();
        mOneShotSwitchEvent.store(0);
        mOneShotSwitchEventName.clear();

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
    mAutoFillJumpArmed = false;
    mAutoFillJumpName.clear();
    mAutoFillJumpGen = -1;
    clearOneShotLocked();
    mOneShotStartedEvent.store(0);
    mOneShotStartedEventName.clear();
    mOneShotSwitchEvent.store(0);
    mOneShotSwitchEventName.clear();
    resetBassMidiLocked(true);
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

bool DrumLoopEngine::preparePlaybackSourceLocked(const std::string& name,
                                                 double logicalStartSec,
                                                 PlaybackSource& out) {
    auto it = mLoops.find(name);
    if (it == mLoops.end()) return false;
    const auto& loop = it->second;

    out = PlaybackSource{};
    out.loop = loop;
    out.buf = loop->buf;
    out.bufGen = loop->bufGen;
    out.sampleRate = loop->sampleRate;
    out.logicalOriginFrame = loop->bufOriginFrame;
    out.logicalDurationFrames = loop->bufLogicalFrames;
    out.complete = loop->complete;
    out.hqState = 0;

    double logicalFrame = std::max(0.0, logicalStartSec) * (double)loop->sampleRate;
    if (out.logicalDurationFrames > 0.0) {
        logicalFrame = std::fmod(logicalFrame, out.logicalDurationFrames);
        if (logicalFrame < 0.0) logicalFrame += out.logicalDurationFrames;
    }
    out.logicalStartFrame = logicalFrame;

    if (loop->hqBuf && loop->hqGen >= 0 && loop->hqState > 0 &&
        loop->hqLogicalFrames > 0.0) {
        const double logicalDuration = loop->hqLogicalFrames;
        logicalFrame = std::max(0.0, logicalStartSec) * (double)loop->sampleRate;
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
        if (allowed && availableFrames >= 2.0 && physical < availableFrames) {
            out.buf = loop->hqBuf;
            out.bufGen = loop->hqGen;
            out.logicalOriginFrame = loop->hqOriginFrame;
            out.logicalDurationFrames = loop->hqLogicalFrames;
            out.logicalStartFrame = logicalFrame;
            out.pos = physical;
            out.complete = loop->hqComplete;
            out.hqState = loop->hqState;
        }
    }

    if (out.hqState == 0 && out.logicalDurationFrames > 0.0) {
        logicalFrame = std::max(0.0, logicalStartSec) * (double)loop->sampleRate;
        logicalFrame = std::fmod(logicalFrame, out.logicalDurationFrames);
        if (logicalFrame < 0.0) logicalFrame += out.logicalDurationFrames;
        out.logicalStartFrame = logicalFrame;
        out.pos = std::fmod(
            logicalFrame - out.logicalOriginFrame + out.logicalDurationFrames,
            out.logicalDurationFrames);
        if (out.pos < 0.0) out.pos += out.logicalDurationFrames;
    }

    if (!out.buf || !out.buf->valid() || out.sampleRate <= 0 ||
        out.logicalDurationFrames <= 0.0) return false;
    const size_t published = out.buf->publishedFrames.load(std::memory_order_acquire);
    return published >= 2 && out.pos < (double)published;
}

void DrumLoopEngine::clearOneShotLocked() {
    mOneShotStartArmed = false;
    mOneShotStartRemainingSec = 0.0;
    mOneShotPendingName.clear();
    mOneShotPendingLogicalStartSec = 0.0;
    mOneShotPendingFadeMs = 30;
    mOneShotPendingNextName.clear();
    mOneShotPendingStopAfter = false;
    mOneShotPendingEndFadeSec = 0.0;
    mOneShotPendingSource = PlaybackSource{};
    mOneShotPendingNext = PlaybackSource{};
    mOneShotActive = false;
    mOneShotStopAfter = false;
    mOneShotName.clear();
    mOneShotRemainingSourceFrames = 0.0;
    mOneShotEndFadeSourceFrames = 0.0;
    mOneShotNext = PlaybackSource{};
    mOneShotNextReady = false;
}

bool DrumLoopEngine::beginOneShotLocked(PlaybackSource source, int fadeMs,
                                        PlaybackSource next, bool stopAfter,
                                        int32_t deviceSampleRate, double endFadeSec) {
    if (!source.loop || !source.buf || !source.buf->valid() || source.sampleRate <= 0) {
        return false;
    }
    const int safeFadeMs = std::max(1, std::min(fadeMs, 250));
    const size_t published = source.buf->publishedFrames.load(std::memory_order_acquire);
    const size_t fadeSourceFrames = (size_t)std::ceil(
        (double)safeFadeMs * (double)source.sampleRate / 1000.0) + 2;
    const size_t startFrame = (size_t)std::floor(source.pos);
    if (startFrame >= published ||
        (!source.complete && published - startFrame < fadeSourceFrames)) return false;

    const int old = mCurSlot;
    const int nu = 1 - old;
    const bool hasOld = (bool)mSlots[old].loop;
    const int32_t deviceRate = std::max<int32_t>(1, deviceSampleRate);
    const int64_t fadeFrames = std::max<int64_t>(
        1, (int64_t)std::llround((double)safeFadeMs * (double)deviceRate / 1000.0));

    if (hasOld) {
        mSlots[old].fading = false;
        mSlots[old].fadeInRemainingUs = 0;
        mSlots[old].gain = 1.0f;
        mSlots[old].constantPowerRole = -1;
        mSlots[old].constantPowerTotalFrames = fadeFrames;
        mSlots[old].constantPowerProgressFrames = 0;
    } else {
        mSlots[old] = Slot{};
    }

    Slot incoming{};
    incoming.loop = source.loop;
    incoming.buf = source.buf;
    incoming.bufGen = source.bufGen;
    incoming.sampleRate = source.sampleRate;
    incoming.logicalOriginFrame = source.logicalOriginFrame;
    incoming.logicalDurationFrames = source.logicalDurationFrames;
    incoming.complete = source.complete;
    incoming.hqState = source.hqState;
    incoming.pos = source.pos;
    incoming.gain = 1.0f;
    incoming.constantPowerRole = hasOld ? 1 : 0;
    incoming.constantPowerTotalFrames = hasOld ? fadeFrames : 0;
    incoming.constantPowerProgressFrames = 0;
    mSlots[nu] = std::move(incoming);
    mCurSlot = nu;

    // 在切入 BREAK 的精确音频边界只关闭当前贝斯音符。这里不能发送
    // All Sounds Off：它会同时硬切合成器里的全部声部，容易产生断音爆音。
    // 同时把 MIDI transport 停在 BREAK 上，避免首个音频回调再次同步并
    // 间接发送 All Sounds Off。BREAK 的 bassMidi 已在注册时清空。
    if (source.loop->name == "BREAK") {
        if (mBassMidiOutputPitch >= 0 && mBassMidiCallback) {
            mBassMidiCallback(BassNoteOff, mBassMidiOutputPitch, 0);
        }
        resetBassMidiLocked(false);
        mBassMidiLoop = source.loop.get();
        mBassMidiBufGen = source.bufGen;
        mBassMidiDurationFrames = source.logicalDurationFrames;
        mBassMidiLastLogicalFrame = source.logicalStartFrame;
        mBassMidiNextEvent = 0;
        mBassMidiNeedsSync = false;
    }

    mTempoCutArmed = false;
    mAutoFillJumpArmed = false;
    mAutoFillJumpName.clear();
    mAutoFillJumpGen = -1;
    mOneShotStartArmed = false;
    mOneShotStartRemainingSec = 0.0;
    mOneShotPendingName.clear();
    mOneShotPendingLogicalStartSec = 0.0;
    mOneShotPendingFadeMs = 30;
    mOneShotPendingNextName.clear();
    mOneShotPendingStopAfter = false;
    mOneShotPendingEndFadeSec = 0.0;
    mOneShotPendingSource = PlaybackSource{};
    mOneShotPendingNext = PlaybackSource{};
    mOneShotActive = true;
    mOneShotStopAfter = stopAfter;
    mOneShotName = source.loop->name;
    mOneShotRemainingSourceFrames = source.logicalDurationFrames - source.logicalStartFrame;
    if (mOneShotRemainingSourceFrames <= 1e-6) {
        mOneShotRemainingSourceFrames += source.logicalDurationFrames;
    }
    mOneShotEndFadeSourceFrames = stopAfter
        ? std::min(mOneShotRemainingSourceFrames,
                   std::max(0.0, endFadeSec) * (double)source.sampleRate)
        : 0.0;
    mOneShotNext = std::move(next);
    mOneShotNextReady = !stopAfter;
    mFading.store(0);
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    const int64_t nowUs = std::chrono::duration_cast<std::chrono::microseconds>(now).count();
    mTriggerUs.store(nowUs);
    mLatencyMs.store(-1);

    __android_log_print(ANDROID_LOG_INFO, "DrumLoop",
                        "one-shot start %s at %.3f fade=%dms next=%s stop=%d state=%d",
                        mOneShotName.c_str(),
                        source.logicalStartFrame / (double)source.sampleRate,
                        safeFadeMs,
                        mOneShotNext.loop ? mOneShotNext.loop->name.c_str() : "",
                        stopAfter ? 1 : 0, source.hqState);
    return true;
}

bool DrumLoopEngine::startOneShot(const std::string& name, double logicalStartSec,
                                  int fadeMs, const std::string& nextName,
                                  bool stopAfter, double delaySec, double endFadeSec) {
    if (name.empty() || logicalStartSec < 0.0 || !std::isfinite(logicalStartSec) ||
        delaySec < 0.0 || !std::isfinite(delaySec) || endFadeSec < 0.0 ||
        !std::isfinite(endFadeSec)) return false;
    const int safeFadeMs = std::max(1, std::min(fadeMs, 250));

    std::lock_guard<std::mutex> lk(mLock);
    PlaybackSource source;
    if (!preparePlaybackSourceLocked(name, logicalStartSec, source)) return false;
    PlaybackSource next;
    if (!stopAfter && (nextName.empty() ||
        !preparePlaybackSourceLocked(nextName, 0.0, next))) return false;

    // 延迟期间也先验证启动点已有完整淡化水位；之后缓冲只会继续增长。
    const size_t published = source.buf->publishedFrames.load(std::memory_order_acquire);
    const size_t fadeSourceFrames = (size_t)std::ceil(
        (double)safeFadeMs * (double)source.sampleRate / 1000.0) + 2;
    const size_t startFrame = (size_t)std::floor(source.pos);
    if (startFrame >= published ||
        (!source.complete && published - startFrame < fadeSourceFrames)) return false;

    clearOneShotLocked();
    mOneShotStartedEvent.store(0);
    mOneShotStartedEventName.clear();
    mOneShotSwitchEvent.store(0);
    mOneShotSwitchEventName.clear();

    if (delaySec <= 1e-6) {
        return beginOneShotLocked(std::move(source), safeFadeMs, std::move(next),
                                  stopAfter, mLastDeviceSampleRate.load(), endFadeSec);
    }

    mTempoCutArmed = false;
    mAutoFillJumpArmed = false;
    mAutoFillJumpName.clear();
    mAutoFillJumpGen = -1;
    mOneShotStartArmed = true;
    mOneShotStartRemainingSec = delaySec;
    mOneShotPendingName = name;
    mOneShotPendingLogicalStartSec = logicalStartSec;
    mOneShotPendingFadeMs = safeFadeMs;
    mOneShotPendingNextName = nextName;
    mOneShotPendingStopAfter = stopAfter;
    mOneShotPendingEndFadeSec = endFadeSec;
    mOneShotPendingSource = std::move(source);
    mOneShotPendingNext = std::move(next);
    __android_log_print(ANDROID_LOG_INFO, "DrumLoop",
                        "one-shot armed %s delay=%.3f start=%.3f stop=%d",
                        name.c_str(), delaySec, logicalStartSec, stopAfter ? 1 : 0);
    return true;
}

bool DrumLoopEngine::fireOneShotStartLocked(int32_t deviceSampleRate) {
    if (!mOneShotStartArmed) return false;
    const std::string name = mOneShotPendingName;
    PlaybackSource source = mOneShotPendingSource;
    PlaybackSource refreshedSource;
    if (preparePlaybackSourceLocked(name, mOneShotPendingLogicalStartSec, refreshedSource)) {
        source = std::move(refreshedSource);
    }
    PlaybackSource next = mOneShotPendingNext;
    if (!mOneShotPendingStopAfter && !mOneShotPendingNextName.empty()) {
        PlaybackSource refreshedNext;
        if (preparePlaybackSourceLocked(mOneShotPendingNextName, 0.0, refreshedNext)) {
            next = std::move(refreshedNext);
        }
    }
    const int fadeMs = mOneShotPendingFadeMs;
    const bool stopAfter = mOneShotPendingStopAfter;
    const double endFadeSec = mOneShotPendingEndFadeSec;
    if (!beginOneShotLocked(std::move(source), fadeMs, std::move(next),
                            stopAfter, deviceSampleRate, endFadeSec)) {
        __android_log_print(ANDROID_LOG_ERROR, "DrumLoop",
                            "delayed one-shot failed at boundary: %s", name.c_str());
        clearOneShotLocked();
        return false;
    }
    mOneShotStartedEventName = name;
    mOneShotStartedEvent.store(1, std::memory_order_release);
    return true;
}

bool DrumLoopEngine::updateOneShotNext(const std::string& nextName) {
    if (nextName.empty()) return false;
    std::lock_guard<std::mutex> lk(mLock);
    if (!mOneShotActive || mOneShotStopAfter) return false;
    PlaybackSource next;
    if (!preparePlaybackSourceLocked(nextName, 0.0, next)) return false;
    mOneShotNext = std::move(next);
    mOneShotNextReady = true;
    __android_log_print(ANDROID_LOG_INFO, "DrumLoop", "one-shot next=%s", nextName.c_str());
    return true;
}

void DrumLoopEngine::cancelOneShot() {
    std::lock_guard<std::mutex> lk(mLock);
    clearOneShotLocked();
    mOneShotStartedEvent.store(0);
    mOneShotStartedEventName.clear();
    mOneShotSwitchEvent.store(0);
    mOneShotSwitchEventName.clear();
}

std::string DrumLoopEngine::getAndClearOneShotStartedEvent() {
    if (!mOneShotStartedEvent.exchange(0)) return {};
    std::lock_guard<std::mutex> lk(mLock);
    return mOneShotStartedEventName;
}

std::string DrumLoopEngine::getAndClearOneShotSwitchEvent() {
    if (!mOneShotSwitchEvent.exchange(0)) return {};
    std::lock_guard<std::mutex> lk(mLock);
    return mOneShotSwitchEventName;
}

int DrumLoopEngine::fireOneShotLocked() {
    if (!mOneShotActive) return 0;
    const bool shouldStop = mOneShotStopAfter || !mOneShotNextReady ||
        !mOneShotNext.loop || !mOneShotNext.buf;
    const std::string finishedName = mOneShotName;

    if (shouldStop) {
        mSlots[0] = Slot{};
        mSlots[1] = Slot{};
        resetBassMidiLocked(true);
        clearOneShotLocked();
        mStoppedEvent.store(1);
        __android_log_print(ANDROID_LOG_INFO, "DrumLoop",
                            "one-shot complete %s -> stop", finishedName.c_str());
        return 2;
    }

    // next 在一次性段落开始时可能只有 250ms 流式水位。播放 Fill/INTRO/BREAK
    // 的几秒内，同一缓冲通常已经完成，或更高的 HQ 状态已发布。结束样本处
    // 重新取一次快照，避免把启动时 complete=false 的旧元数据带入目标槽，
    // 否则目标播完一遍后会误以为缓冲仍未完成而停在末尾。
    PlaybackSource next = mOneShotNext;
    PlaybackSource refreshed;
    if (next.loop && preparePlaybackSourceLocked(next.loop->name, 0.0, refreshed)) {
        next = std::move(refreshed);
    }
    mSlots[0] = Slot{};
    mSlots[1] = Slot{};
    Slot slot{};
    slot.loop = next.loop;
    slot.buf = next.buf;
    slot.bufGen = next.bufGen;
    slot.sampleRate = next.sampleRate;
    slot.logicalOriginFrame = next.logicalOriginFrame;
    slot.logicalDurationFrames = next.logicalDurationFrames;
    slot.complete = next.complete;
    slot.hqState = next.hqState;
    slot.pos = next.pos;
    slot.gain = 1.0f;
    mSlots[0] = std::move(slot);
    mCurSlot = 0;
    mOneShotSwitchEventName = next.loop->name;
    mOneShotSwitchEvent.store(1, std::memory_order_release);
    clearOneShotLocked();
    __android_log_print(ANDROID_LOG_INFO, "DrumLoop",
                        "one-shot complete %s -> %s",
                        finishedName.c_str(), mOneShotSwitchEventName.c_str());
    return 1;
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
    mAutoFillJumpArmed = false;
    mAutoFillJumpName.clear();
    mAutoFillJumpGen = -1;
    mAutoFillJumpRemainingSec = 0.0;
    mAutoFillJumpDestinationSec = 0.0;
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

bool DrumLoopEngine::armAutoFillJump(const std::string& name, int gen, double delaySec,
                                     double destinationLogicalSec) {
    if (name.empty() || gen < 0 || delaySec < 0.0 ||
        !std::isfinite(delaySec) || destinationLogicalSec < 0.0 ||
        !std::isfinite(destinationLogicalSec)) return false;

    std::lock_guard<std::mutex> lk(mLock);
    const Slot& current = mSlots[mCurSlot];
    auto it = mLoops.find(name);
    if (mTempoCutArmed || !current.loop || current.loop->name != name ||
        it == mLoops.end()) return false;
    const auto& loop = it->second;
    if (!loop->hqBuf || !loop->hqBuf->valid() || loop->hqGen != gen ||
        loop->hqState < 2 || loop->hqLogicalFrames <= 0.0 || loop->sampleRate <= 0) {
        return false;
    }

    double logicalFrame = destinationLogicalSec * (double)loop->sampleRate;
    logicalFrame = std::fmod(logicalFrame, loop->hqLogicalFrames);
    if (logicalFrame < 0.0) logicalFrame += loop->hqLogicalFrames;
    // state2 只保证旋转缓冲中的最后一小节和开头 0.5s；目的点必须属于尾段。
    if (loop->hqState < 3 && logicalFrame + 1.0 < loop->hqOriginFrame) return false;
    double physical = std::fmod(
        logicalFrame - loop->hqOriginFrame + loop->hqLogicalFrames,
        loop->hqLogicalFrames);
    if (physical < 0.0) physical += loop->hqLogicalFrames;
    const size_t published = loop->hqBuf->publishedFrames.load(std::memory_order_acquire);
    const size_t startFrame = (size_t)std::llround(physical);
    const size_t fadeFrames = (size_t)std::ceil(
        (double)kSwitchFadeUs * (double)loop->sampleRate / 1000000.0);
    if (startFrame >= published || published - startFrame < fadeFrames + 2) return false;

    mAutoFillJumpArmed = true;
    mAutoFillJumpName = name;
    mAutoFillJumpGen = gen;
    mAutoFillJumpRemainingSec = delaySec;
    mAutoFillJumpDestinationSec = destinationLogicalSec;
    __android_log_print(ANDROID_LOG_INFO, "DrumLoop",
                        "auto fill armed %s gen=%d delay=%.3f destination=%.3f state=%d",
                        name.c_str(), gen, delaySec, destinationLogicalSec, loop->hqState);
    return true;
}

void DrumLoopEngine::cancelAutoFillJump() {
    std::lock_guard<std::mutex> lk(mLock);
    mAutoFillJumpArmed = false;
    mAutoFillJumpName.clear();
    mAutoFillJumpGen = -1;
    mAutoFillJumpRemainingSec = 0.0;
    mAutoFillJumpDestinationSec = 0.0;
}

bool DrumLoopEngine::fireAutoFillJumpLocked(int32_t deviceSampleRate) {
    if (!mAutoFillJumpArmed) return false;
    auto it = mLoops.find(mAutoFillJumpName);
    Slot& current = mSlots[mCurSlot];
    if (!current.loop || current.loop->name != mAutoFillJumpName ||
        it == mLoops.end()) {
        mAutoFillJumpArmed = false;
        return false;
    }
    const auto& loop = it->second;
    if (!loop->hqBuf || !loop->hqBuf->valid() || loop->hqGen != mAutoFillJumpGen ||
        loop->hqState < 2 || loop->hqLogicalFrames <= 0.0 || loop->sampleRate <= 0) {
        mAutoFillJumpArmed = false;
        return false;
    }

    double logicalFrame = mAutoFillJumpDestinationSec * (double)loop->sampleRate;
    logicalFrame = std::fmod(logicalFrame, loop->hqLogicalFrames);
    if (logicalFrame < 0.0) logicalFrame += loop->hqLogicalFrames;
    if (loop->hqState < 3 && logicalFrame + 1.0 < loop->hqOriginFrame) {
        mAutoFillJumpArmed = false;
        return false;
    }
    double physical = std::fmod(
        logicalFrame - loop->hqOriginFrame + loop->hqLogicalFrames,
        loop->hqLogicalFrames);
    if (physical < 0.0) physical += loop->hqLogicalFrames;
    const size_t published = loop->hqBuf->publishedFrames.load(std::memory_order_acquire);
    const size_t startFrame = (size_t)std::llround(physical);
    const size_t requiredSourceFrames = (size_t)std::ceil(
        (double)kSwitchFadeUs * (double)loop->sampleRate / 1000000.0) + 2;
    if (startFrame >= published || published - startFrame < requiredSourceFrames) {
        mAutoFillJumpArmed = false;
        return false;
    }

    const int old = mCurSlot;
    const int nu = 1 - old;
    const int64_t fadeFrames = std::max<int64_t>(
        1, (int64_t)std::llround((double)kSwitchFadeUs *
                                 (double)std::max(1, deviceSampleRate) / 1000000.0));

    // A：保留当前读指针并使用 cos 曲线淡出。
    mSlots[old].fading = false;
    mSlots[old].fadeInRemainingUs = 0;
    mSlots[old].gain = 1.0f;
    mSlots[old].constantPowerRole = -1;
    mSlots[old].constantPowerTotalFrames = fadeFrames;
    mSlots[old].constantPowerProgressFrames = 0;

    // B：同一个 variation 的 state2/3 快照，从最后一小节配对切点开始，以 sin 曲线淡入。
    Slot next{};
    next.loop = loop;
    next.buf = loop->hqBuf;
    next.bufGen = loop->hqGen;
    next.sampleRate = loop->sampleRate;
    next.logicalOriginFrame = loop->hqOriginFrame;
    next.logicalDurationFrames = loop->hqLogicalFrames;
    next.complete = loop->hqComplete;
    next.hqState = loop->hqState;
    next.pos = physical;
    next.gain = 1.0f;
    next.constantPowerRole = 1;
    next.constantPowerTotalFrames = fadeFrames;
    next.constantPowerProgressFrames = 0;
    mSlots[nu] = std::move(next);
    mCurSlot = nu;

    const int firedState = loop->hqState;
    __android_log_print(ANDROID_LOG_INFO, "DrumLoop",
                        "auto fill fired %s destination=%.3f physical=%.3f state=%d fadeFrames=%lld",
                        mAutoFillJumpName.c_str(), logicalFrame / (double)loop->sampleRate,
                        physical / (double)loop->sampleRate, firedState,
                        (long long)fadeFrames);
    mAutoFillJumpArmed = false;
    mAutoFillJumpName.clear();
    mAutoFillJumpGen = -1;
    mAutoFillJumpRemainingSec = 0.0;
    mAutoFillJumpDestinationSec = 0.0;
    return true;
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

// ================= 贝斯 MIDI transport =================

void DrumLoopEngine::clearRetainedBassChordLocked() {
    mChordBassPitch.store(-1, std::memory_order_relaxed);
    mBassChordPitches.clear();
    mBassChordTonePcs.clear();
    mBassChordRootPc = -1;
    mBassChordThirdPc = -1;
    mBassChordFifthPc = -1;
    mBassConnectedGroup = -1;
    mBassConnectedPreviousPitch = -1;
    if (mBassMidiOutputPitch >= 0 && mBassMidiCallback) {
        mBassMidiCallback(BassNoteOff, mBassMidiOutputPitch, 0);
    }
    mBassMidiOutputPitch = -1;
}

void DrumLoopEngine::bassChordTimerLoop() {
    std::unique_lock<std::mutex> lk(mLock);
    while (!mBassChordTimerStopping) {
        if (mGlobalBassChordPresent || mBassNoChordDeadlineUs < 0) {
            mBassChordTimerCv.wait(lk, [this]() {
                return mBassChordTimerStopping ||
                    (!mGlobalBassChordPresent && mBassNoChordDeadlineUs >= 0);
            });
            continue;
        }

        const int64_t expectedDeadlineUs = mBassNoChordDeadlineUs;
        const auto deadline = std::chrono::steady_clock::time_point(
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::microseconds(expectedDeadlineUs)));
        const bool cancelled = mBassChordTimerCv.wait_until(
            lk, deadline, [this, expectedDeadlineUs]() {
                return mBassChordTimerStopping || mGlobalBassChordPresent ||
                    mBassNoChordDeadlineUs != expectedDeadlineUs;
            });
        if (cancelled || mBassChordTimerStopping) continue;

        if (!mGlobalBassChordPresent &&
            mBassNoChordDeadlineUs == expectedDeadlineUs &&
            steadyNowMicros() >= expectedDeadlineUs) {
            mBassNoChordDeadlineUs = -1;
            clearRetainedBassChordLocked();
            __android_log_print(ANDROID_LOG_INFO, "DrumLoop",
                                "global chord empty for 5s; bass chord cleared");
        }
    }
}

void DrumLoopEngine::resetBassMidiLocked(bool emitAllSoundsOff) {
    if (emitAllSoundsOff && mBassMidiCallback) {
        mBassMidiCallback(BassAllSoundsOff, -1, 0);
    }
    mBassMidiLoop = nullptr;
    mBassMidiBufGen = -1;
    mBassMidiDurationFrames = 0.0;
    mBassMidiLastLogicalFrame = -1.0;
    mBassMidiNextEvent = 0;
    mBassMidiNeedsSync = true;
    mBassMidiInputOn = false;
    mBassMidiInputVelocity = 0;
    mBassMidiInputDownbeat = false;
    mBassMidiInputStrongBeat = false;
    mBassMidiInputKind = 0;
    mBassMidiInputGroup = -1;
    mBassMidiGateStartUs = -1;
    mBassMidiGateEndLogicalFrame = -1.0;
    mBassMidiOutputPitch = -1;
    mBassConnectedGroup = -1;
    mBassConnectedPreviousPitch = -1;
}

float DrumLoopEngine::nextBassRandomLocked() {
    // Small allocation-free xorshift generator. All callers already hold mLock.
    uint32_t x = mBassRandomState;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    mBassRandomState = x ? x : 0x6d2b79f5u;
    return (float)(mBassRandomState & 0x00ffffffu) / 16777216.0f;
}

int DrumLoopEngine::chooseHighC5PitchLocked(int phraseGroup) {
    if (mBassChordRootPc < 0) return -1;

    // phraseGroup>=0 表示一拍范围内相邻的 C5 乐句；孤立 C5 不继承上次音高。
    if (phraseGroup < 0) {
        mBassConnectedGroup = -1;
        mBassConnectedPreviousPitch = -1;
    } else if (phraseGroup != mBassConnectedGroup) {
        mBassConnectedGroup = phraseGroup;
        mBassConnectedPreviousPitch = -1;
    }

    std::vector<int> candidates;
    candidates.reserve(mBassChordTonePcs.size());
    for (int pc : mBassChordTonePcs) {
        const int interval = (normalizeBassPc(pc) - mBassChordRootPc + 12) % 12;
        // 根、三、五、七、九音；9 也包含 dim7 的减七音，1/2/3 覆盖 b9/9/#9。
        const bool supportedRole = interval == 0 || interval == 1 || interval == 2 ||
            interval == 3 || interval == 4 || interval == 6 || interval == 7 ||
            interval == 8 || interval == 9 || interval == 10 || interval == 11;
        if (!supportedRole) continue;

        const int base = pitchClassInARegister(pc, kBassHighRegisterStart);
        int voiced = base;
        if (mBassConnectedPreviousPitch >= 0) {
            int bestDistance = std::abs(voiced - mBassConnectedPreviousPitch);
            for (int octave = -1; octave <= 2; ++octave) {
                const int candidate = base + octave * 12;
                if (candidate < kBassHighRegisterStart || candidate > kBassHighRegisterEnd) {
                    continue;
                }
                const int distance = std::abs(candidate - mBassConnectedPreviousPitch);
                if (distance < bestDistance) {
                    voiced = candidate;
                    bestDistance = distance;
                }
            }
        }
        if (std::find(candidates.begin(), candidates.end(), voiced) == candidates.end()) {
            candidates.push_back(voiced);
        }
    }
    if (candidates.empty()) {
        candidates.push_back(
            pitchClassInARegister(mBassChordRootPc, kBassHighRegisterStart));
    }

    std::vector<float> weights(candidates.size(), 1.0f);
    int minimumDistance = 0;
    if (mBassConnectedPreviousPitch >= 0) {
        minimumDistance = 128;
        for (int pitch : candidates) {
            minimumDistance = std::min(
                minimumDistance, std::abs(pitch - mBassConnectedPreviousPitch));
        }
        for (size_t i = 0; i < candidates.size(); ++i) {
            const int distance = std::abs(
                candidates[i] - mBassConnectedPreviousPitch);
            // 邻近乐句硬性限制在七个半音内，并只保留离最佳进行最多三
            // 个半音的候选；剩余候选再按距离平方反比抽取。
            if (distance > 7 || distance > minimumDistance + 3) {
                weights[i] = 0.0f;
            } else {
                const float divisor = (float)(distance + 1);
                weights[i] = 1.0f / (divisor * divisor);
            }
        }
    }

    float totalWeight = 0.0f;
    for (float weight : weights) totalWeight += weight;
    if (totalWeight <= 0.0f) {
        size_t nearest = 0;
        for (size_t i = 1; i < candidates.size(); ++i) {
            if (std::abs(candidates[i] - mBassConnectedPreviousPitch) <
                std::abs(candidates[nearest] - mBassConnectedPreviousPitch)) {
                nearest = i;
            }
        }
        weights[nearest] = 1.0f;
        totalWeight = 1.0f;
    }
    float draw = nextBassRandomLocked() * totalWeight;
    size_t selected = candidates.size() - 1;
    for (size_t i = 0; i < candidates.size(); ++i) {
        draw -= weights[i];
        if (draw <= 0.0f) {
            selected = i;
            break;
        }
    }

    // 浮点边界兜底：绝不让被置零的大跳候选成为结果。
    if (weights[selected] <= 0.0f) {
        selected = 0;
        for (size_t i = 1; i < candidates.size(); ++i) {
            if (weights[i] > weights[selected]) selected = i;
        }
    }

    const int pitch = candidates[selected];
    if (phraseGroup >= 0) mBassConnectedPreviousPitch = pitch;
    return pitch;
}

int DrumLoopEngine::chooseBassPitchLocked(int noteKind, bool downbeat,
                                          int phraseGroup) {
    if (noteKind == 1 || noteKind == 2) return chooseHighC5PitchLocked(phraseGroup);

    // The first beat of every measure is deterministic: always use the current
    // chord root/slash bass. C4 off-beats retain the original BassAssist-derived
    // root/third/fifth selection algorithm.
    if (downbeat) return mChordBassPitch.load(std::memory_order_relaxed);
    if (mBassChordPitches.empty()) {
        return mChordBassPitch.load(std::memory_order_relaxed);
    }
    if (mBassChordPitches.size() == 1) return mBassChordPitches.front();

    // Copy BassAssist's off-beat selection probabilities. Energy changes the
    // probability only; it never scales the authored MIDI velocity.
    const float energy = mBassSelectionEnergy.load(std::memory_order_relaxed);

    const float random01 = nextBassRandomLocked();

    if (mBassChordPitches.size() == 2) {
        // Two-note/power chord: up to 40% for the fifth at maximum energy.
        const float firstProbability = 1.0f - 0.4f * energy;
        return random01 < firstProbability
            ? mBassChordPitches[0] : mBassChordPitches[1];
    }

    // Same distribution as BassAssist: at maximum energy, 60% bass/root,
    // 10% second chord candidate, 30% third candidate (normally the fifth).
    const float firstProbability = 1.0f - 0.4f * energy;
    const float secondProbability = 0.1f * energy;
    if (random01 < firstProbability) return mBassChordPitches[0];
    if (random01 < firstProbability + secondProbability) return mBassChordPitches[1];
    return mBassChordPitches[2];
}

int DrumLoopEngine::bassCatchUpVelocityLocked() const {
    if (!mBassMidiInputOn || mBassMidiInputVelocity <= 0 || mBassMidiGateStartUs < 0) {
        return 0;
    }
    const double elapsedMs = std::max(
        0.0, (double)(steadyNowMicros() - mBassMidiGateStartUs) / 1000.0);
    const double thirtySecondMs = 7500.0 / std::max(30.0, mBassTempoBpm);
    const double elapsedThirtySeconds = elapsedMs / thirtySecondMs;
    const double curvedTime = std::pow(
        std::max(0.0, elapsedThirtySeconds), kBassCatchUpCurveExponent);
    const double remaining = std::pow(kBassCatchUpAtThirtySecond, curvedTime);
    return std::max(0, std::min(127,
        (int)std::lround((double)mBassMidiInputVelocity * remaining)));
}

int DrumLoopEngine::bassOutputVelocityLocked(int noteKind, int sourceVelocity) const {
    sourceVelocity = std::max(0, std::min(127, sourceVelocity));
    if (noteKind == 0 || sourceVelocity == 0) return sourceVelocity;
    return std::max(1, std::min(127,
        (int)std::lround((double)sourceVelocity * kBassHighVelocityScale)));
}

double DrumLoopEngine::findBassGateEndFrameLocked(const Loop& loop,
                                                   size_t noteOnIndex) const {
    const auto& events = loop.bassMidi;
    if (noteOnIndex >= events.size() || events[noteOnIndex].velocity <= 0 ||
        mBassMidiDurationFrames <= 0.0) return -1.0;
    const auto& noteOn = events[noteOnIndex];
    for (size_t offset = 1; offset < events.size(); ++offset) {
        const auto& candidate = events[(noteOnIndex + offset) % events.size()];
        if (candidate.velocity != 0 || candidate.noteKind != noteOn.noteKind) continue;
        if (noteOn.noteKind == 2 && candidate.phraseGroup != noteOn.phraseGroup) continue;
        return candidate.phase * mBassMidiDurationFrames;
    }
    return -1.0;
}

double DrumLoopEngine::bassGateRemainingBeatsLocked() const {
    if (!mBassMidiInputOn || !mBassMidiLoop || mBassMidiDurationFrames <= 0.0 ||
        mBassMidiGateEndLogicalFrame < 0.0 || mBassMidiLastLogicalFrame < 0.0 ||
        mBassMidiLoop->sampleRate <= 0) return 0.0;
    double current = std::fmod(mBassMidiLastLogicalFrame, mBassMidiDurationFrames);
    double end = std::fmod(mBassMidiGateEndLogicalFrame, mBassMidiDurationFrames);
    if (current < 0.0) current += mBassMidiDurationFrames;
    if (end < 0.0) end += mBassMidiDurationFrames;
    double remainingFrames = end - current;
    if (remainingFrames < 0.0) remainingFrames += mBassMidiDurationFrames;
    if (remainingFrames <= 0.5) return 0.0;
    const double remainingSec = remainingFrames / (double)mBassMidiLoop->sampleRate;
    return remainingSec * std::max(30.0, mBassTempoBpm) / 60.0;
}

bool DrumLoopEngine::bassPitchFitsCurrentTriadLocked(int pitch) const {
    if (pitch < 0 || mBassChordRootPc < 0) return false;
    const int pc = normalizeBassPc(pitch);
    return pc == mBassChordRootPc || pc == mBassChordThirdPc ||
           pc == mBassChordFifthPc;
}

void DrumLoopEngine::emitBassMidiEventLocked(const Loop& loop, size_t eventIndex) {
    if (eventIndex >= loop.bassMidi.size()) return;
    const auto& event = loop.bassMidi[eventIndex];
    if (event.velocity > 0) {
        if (mBassMidiOutputPitch >= 0 && mBassMidiCallback) {
            mBassMidiCallback(BassNoteOff, mBassMidiOutputPitch, 0);
        }
        mBassMidiInputOn = true;
        mBassMidiInputVelocity = event.velocity;
        mBassMidiInputDownbeat = event.downbeat;
        mBassMidiInputStrongBeat = event.strongBeat;
        mBassMidiInputKind = event.noteKind;
        mBassMidiInputGroup = event.phraseGroup;
        mBassMidiGateStartUs = steadyNowMicros();
        mBassMidiGateEndLogicalFrame = findBassGateEndFrameLocked(loop, eventIndex);
        mBassMidiOutputPitch = -1;
        const int pitch = chooseBassPitchLocked(
            event.noteKind, event.downbeat, event.phraseGroup);
        if (pitch >= 0 && mBassLoopEnabled.load(std::memory_order_relaxed) &&
            mBassMidiCallback) {
            mBassMidiCallback(
                BassNoteOn, pitch,
                bassOutputVelocityLocked(event.noteKind, event.velocity));
            mBassMidiOutputPitch = pitch;
        }
    } else {
        mBassMidiInputOn = false;
        mBassMidiInputVelocity = 0;
        mBassMidiInputDownbeat = false;
        mBassMidiInputStrongBeat = false;
        mBassMidiInputKind = 0;
        mBassMidiInputGroup = -1;
        mBassMidiGateStartUs = -1;
        mBassMidiGateEndLogicalFrame = -1.0;
        if (mBassMidiOutputPitch >= 0 && mBassMidiCallback) {
            mBassMidiCallback(BassNoteOff, mBassMidiOutputPitch, 0);
        }
        mBassMidiOutputPitch = -1;
    }
}

void DrumLoopEngine::syncBassMidiLocked(const Slot& slot, double logicalFrame) {
    if (mBassMidiCallback) mBassMidiCallback(BassAllSoundsOff, -1, 0);
    mBassMidiLoop = slot.loop.get();
    mBassMidiBufGen = slot.bufGen;
    mBassMidiDurationFrames = slot.logicalDurationFrames;
    mBassMidiLastLogicalFrame = logicalFrame - 1.0;
    mBassMidiNextEvent = 0;
    mBassMidiNeedsSync = false;
    mBassMidiInputOn = false;
    mBassMidiInputVelocity = 0;
    mBassMidiInputDownbeat = false;
    mBassMidiInputStrongBeat = false;
    mBassMidiInputKind = 0;
    mBassMidiInputGroup = -1;
    mBassMidiGateStartUs = -1;
    mBassMidiGateEndLogicalFrame = -1.0;
    mBassMidiOutputPitch = -1;
    mBassConnectedGroup = -1;
    mBassConnectedPreviousPitch = -1;
    if (!slot.loop || slot.logicalDurationFrames <= 0.0) return;

    // Starting in the middle of a phrase must preserve the note's remaining duration.
    // Reconstruct whether the reference note is held immediately before the new position.
    const auto& events = slot.loop->bassMidi;
    double activeStartFrame = -1.0;
    size_t activeStartIndex = events.size();
    while (mBassMidiNextEvent < events.size()) {
        const size_t eventIndex = mBassMidiNextEvent;
        const auto& event = events[eventIndex];
        const double eventFrame = event.phase * slot.logicalDurationFrames;
        if (eventFrame >= logicalFrame - 0.5) break;
        mBassMidiInputOn = event.velocity > 0;
        mBassMidiInputVelocity = event.velocity;
        mBassMidiInputDownbeat = event.velocity > 0 && event.downbeat;
        mBassMidiInputStrongBeat = event.velocity > 0 && event.strongBeat;
        mBassMidiInputKind = event.noteKind;
        mBassMidiInputGroup = event.phraseGroup;
        activeStartFrame = event.velocity > 0 ? eventFrame : -1.0;
        activeStartIndex = event.velocity > 0 ? eventIndex : events.size();
        ++mBassMidiNextEvent;
    }
    if (mBassMidiInputOn) {
        const double elapsedFrames = std::max(0.0, logicalFrame - activeStartFrame);
        const double elapsedSec = elapsedFrames / (double)std::max(1, slot.sampleRate);
        mBassMidiGateStartUs = steadyNowMicros() -
            (int64_t)std::llround(elapsedSec * 1000000.0);
        mBassMidiGateEndLogicalFrame = activeStartIndex < events.size()
            ? findBassGateEndFrameLocked(*slot.loop, activeStartIndex) : -1.0;
        const int pitch = chooseBassPitchLocked(
            mBassMidiInputKind, mBassMidiInputDownbeat, mBassMidiInputGroup);
        if (pitch >= 0 && mBassLoopEnabled.load(std::memory_order_relaxed) &&
            mBassMidiCallback) {
            mBassMidiCallback(
                BassNoteOn, pitch,
                bassOutputVelocityLocked(mBassMidiInputKind, mBassMidiInputVelocity));
            mBassMidiOutputPitch = pitch;
        }
    }
}

void DrumLoopEngine::processBassMidiLocked(const Slot& slot, double logicalFrame, double step) {
    if (!slot.loop || slot.logicalDurationFrames <= 0.0) return;
    if (!mBassLoopEnabled.load(std::memory_order_relaxed)) {
        if (mBassMidiLoop || mBassMidiOutputPitch >= 0) resetBassMidiLocked(true);
        return;
    }

    const double duration = slot.logicalDurationFrames;
    logicalFrame = std::fmod(logicalFrame, duration);
    if (logicalFrame < 0.0) logicalFrame += duration;

    // BREAK 永远不演奏贝斯，也不能通过常规 transport 同步触发
    // BassAllSoundsOff。非 one-shot 路径若直接进入 BREAK，也只关闭当前音符。
    if (slot.loop->name == "BREAK") {
        const bool needsBreakMute = mBassMidiLoop != slot.loop.get() ||
                                    mBassMidiNeedsSync ||
                                    mBassMidiOutputPitch >= 0;
        if (needsBreakMute) {
            if (mBassMidiOutputPitch >= 0 && mBassMidiCallback) {
                mBassMidiCallback(BassNoteOff, mBassMidiOutputPitch, 0);
            }
            resetBassMidiLocked(false);
            mBassMidiLoop = slot.loop.get();
            mBassMidiBufGen = slot.bufGen;
            mBassMidiDurationFrames = duration;
            mBassMidiNextEvent = 0;
            mBassMidiNeedsSync = false;
        }
        mBassMidiLastLogicalFrame = logicalFrame;
        return;
    }

    const bool identityChanged = mBassMidiLoop != slot.loop.get() ||
        std::abs(mBassMidiDurationFrames - duration) > 1.0;
    if (identityChanged || mBassMidiNeedsSync) {
        syncBassMidiLocked(slot, logicalFrame);
    } else {
        const double tolerance = std::max(0.75, std::abs(step) * 1.5);
        if (logicalFrame + tolerance < mBassMidiLastLogicalFrame) {
            const bool naturalWrap = mBassMidiLastLogicalFrame > duration * 0.75 &&
                                     logicalFrame < duration * 0.25;
            if (!naturalWrap) {
                syncBassMidiLocked(slot, logicalFrame);
            } else {
                const auto& events = slot.loop->bassMidi;
                while (mBassMidiNextEvent < events.size()) {
                    const size_t eventIndex = mBassMidiNextEvent++;
                    emitBassMidiEventLocked(*slot.loop, eventIndex);
                }
                mBassMidiNextEvent = 0;
                mBassMidiLastLogicalFrame = -tolerance;
                mBassConnectedGroup = -1;
                mBassConnectedPreviousPitch = -1;
            }
        } else if (logicalFrame - mBassMidiLastLogicalFrame >
                   std::max(64.0, std::abs(step) * 4.0)) {
            // A/B jump, one-shot switch, or another non-contiguous transport move.
            syncBassMidiLocked(slot, logicalFrame);
        }
    }

    const auto& events = slot.loop->bassMidi;
    const double tolerance = std::max(0.75, std::abs(step) * 1.5);
    while (mBassMidiNextEvent < events.size()) {
        const auto& event = events[mBassMidiNextEvent];
        const double eventFrame = event.phase * duration;
        if (eventFrame > logicalFrame + tolerance) break;
        const size_t eventIndex = mBassMidiNextEvent++;
        emitBassMidiEventLocked(*slot.loop, eventIndex);
    }
    mBassMidiLastLogicalFrame = logicalFrame;
}

// ================= 混音 (双槽) =================

void DrumLoopEngine::mixAudio(float* outBuf, int32_t numFrames, int32_t deviceSampleRate) {
    float vol = mVolume.load();
    if (!outBuf || numFrames <= 0) return;
    if (deviceSampleRate <= 0) deviceSampleRate = 48000;
    mLastDeviceSampleRate.store(deviceSampleRate, std::memory_order_relaxed);
    double appliedBpm = -1.0;
    TempoCutCallback callback = nullptr;
    int oneShotResult = 0;
    OneShotSwitchCallback oneShotCallback = nullptr;
    bool oneShotStarted = false;
    OneShotSwitchCallback oneShotStartedCallback = nullptr;
    {
        std::lock_guard<std::mutex> lk(mLock);

        // 全局淡出 (停止按钮)
        float masterFade = 1.0f;
        if (mFading.load()) {
            const bool any = (bool)mSlots[0].loop || (bool)mSlots[1].loop;
            if (!any) {
                mFading.store(0);
                clearOneShotLocked();
                resetBassMidiLocked(true);
                mStoppedEvent.store(1);
                return;
            }
            mFadeRemainingUs -= (int64_t)(1000000.0 * (double)numFrames / (double)deviceSampleRate);
            if (mFadeRemainingUs <= 0) {
                mSlots[0].loop.reset();
                mSlots[1].loop.reset();
                mFading.store(0);
                clearOneShotLocked();
                resetBassMidiLocked(true);
                mStoppedEvent.store(1);
                mTempoCutArmed = false;
                __android_log_print(ANDROID_LOG_INFO, "DrumLoop", "fadeOut complete");
                return;
            }
            masterFade = (float)((double)mFadeRemainingUs / (double)mFadeTotalUs);
        }
        const auto mixSegment = [&](float* dst, int32_t frames) {
            if (frames <= 0) return;
            mixSlot(mSlots[1 - mCurSlot], false, dst, frames,
                    deviceSampleRate, vol * masterFade);
            mixSlot(mSlots[mCurSlot], true, dst, frames,
                    deviceSampleRate, vol * masterFade);
        };

        // 变速、Auto Fill、延迟的一次性段落启动及其结束都由真实输出帧倒计时，
        // 并在同一 Oboe 回调内切开缓冲，不依赖 Kotlin/JS 轮询。
        const bool tempoAction = mTempoCutArmed;
        const bool autoFillAction = !tempoAction && mAutoFillJumpArmed;
        const bool oneShotStartAction = !tempoAction && !autoFillAction &&
            mOneShotStartArmed && mSlots[mCurSlot].loop;
        const bool oneShotEndAction = !tempoAction && !autoFillAction &&
            !oneShotStartAction &&
            mOneShotActive && mSlots[mCurSlot].loop;
        int32_t beforeAction = numFrames;
        if (tempoAction || autoFillAction || oneShotStartAction || oneShotEndAction) {
            double remaining = 0.0;
            if (tempoAction) {
                remaining = mTempoCutRemainingSec;
            } else if (autoFillAction) {
                remaining = mAutoFillJumpRemainingSec;
            } else if (oneShotStartAction) {
                remaining = mOneShotStartRemainingSec;
            } else {
                const Slot& current = mSlots[mCurSlot];
                const double step = (double)std::max(1, current.sampleRate) /
                    (double)deviceSampleRate * (double)mRate.load();
                remaining = step > 0.0
                    ? mOneShotRemainingSourceFrames / step / (double)deviceSampleRate
                    : 0.0;
            }
            const double exactFrames = std::max(0.0, remaining) *
                                       (double)deviceSampleRate;
            beforeAction = (int32_t)std::min<double>(
                (double)numFrames, std::ceil(exactFrames));
        }

        mixSegment(outBuf, beforeAction);
        if (tempoAction && mTempoCutArmed) {
            mTempoCutRemainingSec = std::max(
                0.0, mTempoCutRemainingSec -
                    (double)beforeAction / (double)deviceSampleRate);
            if (mTempoCutRemainingSec <= 0.0) {
                appliedBpm = fireTempoCutLocked();
                if (appliedBpm > 0.0) callback = mTempoCutCallback;
            }
        } else if (autoFillAction && mAutoFillJumpArmed) {
            mAutoFillJumpRemainingSec = std::max(
                0.0, mAutoFillJumpRemainingSec -
                    (double)beforeAction / (double)deviceSampleRate);
            if (mAutoFillJumpRemainingSec <= 0.0) {
                fireAutoFillJumpLocked(deviceSampleRate);
            }
        } else if (oneShotStartAction && mOneShotStartArmed) {
            mOneShotStartRemainingSec = std::max(
                0.0, mOneShotStartRemainingSec -
                    (double)beforeAction / (double)deviceSampleRate);
            if (mOneShotStartRemainingSec <= 0.0) {
                oneShotStarted = fireOneShotStartLocked(deviceSampleRate);
                if (oneShotStarted) oneShotStartedCallback = mOneShotSwitchCallback;
            }
        } else if (oneShotEndAction && mOneShotActive) {
            const Slot& current = mSlots[mCurSlot];
            const double step = (double)std::max(1, current.sampleRate) /
                (double)deviceSampleRate * (double)mRate.load();
            mOneShotRemainingSourceFrames = std::max(
                0.0, mOneShotRemainingSourceFrames - (double)beforeAction * step);
            if (mOneShotRemainingSourceFrames <= 1e-6) {
                oneShotResult = fireOneShotLocked();
                if (oneShotResult == 1) oneShotCallback = mOneShotSwitchCallback;
            }
        }
        const int32_t afterAction = numFrames - beforeAction;
        mixSegment(outBuf + (size_t)beforeAction * 2, afterAction);
    }

    // 回调放在播放锁之外；只发布原子状态与更新 BeatTracker，不参与 DSP/分配。
    if (appliedBpm > 0.0) {
        mTempoCutEvent.store(appliedBpm);
        if (callback) callback(appliedBpm);
    }
    if (oneShotStarted && oneShotStartedCallback) oneShotStartedCallback();
    if (oneShotResult == 1 && oneShotCallback) oneShotCallback();
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

        if (isCurrent) {
            double logicalFrame = slot.logicalOriginFrame + pos;
            if (slot.logicalDurationFrames > 0.0) {
                logicalFrame = std::fmod(logicalFrame, slot.logicalDurationFrames);
                if (logicalFrame < 0.0) logicalFrame += slot.logicalDurationFrames;
            }
            processBassMidiLocked(slot, logicalFrame, step);
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

        float frameGain = gain;
        // ENDING 在结尾最后 1/2 拍线性淡出。倒计按逻辑源帧推进，
        // 所以快慢速与设备采样率不会改变淡出落点。
        if (isCurrent && mOneShotActive && mOneShotStopAfter &&
            mOneShotEndFadeSourceFrames > 0.0) {
            const double remaining = mOneShotRemainingSourceFrames - (double)i * step;
            if (remaining < mOneShotEndFadeSourceFrames) {
                const double fade = std::max(
                    0.0, std::min(1.0, remaining / mOneShotEndFadeSourceFrames));
                frameGain *= (float)fade;
            }
        }
        if (slot.constantPowerRole != 0) {
            const int64_t total = std::max<int64_t>(1, slot.constantPowerTotalFrames);
            const int64_t progress = std::min(
                slot.constantPowerProgressFrames, total - 1);
            const int stepIndex = total <= 1
                ? 30 : (int)(progress * 30 / (total - 1));
            const float envelope = slot.constantPowerRole < 0
                ? kConstantPowerSin[30 - stepIndex]
                : kConstantPowerSin[stepIndex];
            frameGain *= envelope;
        }

        outBuf[i * 2] += li * frameGain;
        outBuf[i * 2 + 1] += ri * frameGain;

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

        if (slot.constantPowerRole != 0) {
            slot.constantPowerProgressFrames++;
            if (slot.constantPowerProgressFrames >= slot.constantPowerTotalFrames) {
                const bool fadeOutDone = slot.constantPowerRole < 0;
                slot.constantPowerRole = 0;
                slot.constantPowerTotalFrames = 0;
                slot.constantPowerProgressFrames = 0;
                if (fadeOutDone) {
                    slot.pos = pos;
                    slot.loop.reset();
                    return;
                }
            }
        }
    }

    slot.pos = pos;
}
