#include "SignalsmithStream.h"

#include "signalsmith-stretch/signalsmith-stretch.h"

#include <cmath>
#include <algorithm>

struct SignalsmithStream::Impl {
    signalsmith::stretch::SignalsmithStretch<float> stretch;
    const float ratio;
    std::vector<float> fed;        // 已喂入原始 PCM (float 交错)
    size_t fedConsumed = 0;        // 已被 process 消费的帧数
    bool finished = false;
    size_t produced = 0;           // 已产出帧数
    size_t outTarget = 0;          // 目标总输出帧数
    // 暂存缓冲 (交错 → 平面)
    std::vector<float> inL, inR, outL, outR;

    Impl(int sampleRate, float r) : ratio(r) {
        // 小窗低延迟配置 (约 20ms 块 / 5ms 步进)
//        stretch.configure(2, sampleRate / 50, sampleRate / 200, false); //效果没有原生的presetDefault好
        stretch.configure(2, 960, 120, false);
//        stretch.presetDefault(2, sampleRate);
        stretch.setTransposeSemitones(0);
    }
};

SignalsmithStream::SignalsmithStream(int sampleRate, float ratio)
    : imp(std::make_unique<Impl>(sampleRate, ratio)) {}

SignalsmithStream::~SignalsmithStream() = default;

void SignalsmithStream::feed(const int16_t* data, size_t frames) {
    if (!data || frames == 0 || imp->finished) return;
    const size_t base = imp->fed.size();
    imp->fed.resize(base + frames * 2);
    for (size_t i = 0; i < frames * 2; i++)
        imp->fed[base + i] = data[i] * (1.0f / 32768.0f);
}

void SignalsmithStream::finishInput() {
    if (imp->finished) return;
    // 追加 inputLatency 帧静音，让处理时间推进到输入末尾（官方 README 建议）
    const size_t pad = (size_t)std::max(0, imp->stretch.inputLatency());
    imp->fed.resize(imp->fed.size() + pad * 2, 0.0f);
    // outTarget 基于“原始输入 + 静音补丁”的总帧数计算，已包含补丁部分
    imp->outTarget = (size_t)((double)(imp->fed.size() / 2) / (double)imp->ratio + 0.5);
    imp->finished = true;
}

bool SignalsmithStream::inputFinished() const { return imp->finished; }

//size_t SignalsmithStream::render(int16_t* out, size_t maxFrames) {
//    if (!out || maxFrames == 0) return 0;
//    Impl& p = *imp;
//    const float kScale = 32768.0f;
//    size_t produced = 0;
//
//    while (produced < maxFrames) {
//        if (p.fedConsumed < p.fed.size() / 2) {
//            // 还有输入待处理 (含静音补丁): 用 process 推进
//            const size_t avail = p.fed.size() / 2 - p.fedConsumed;
//            if (!p.finished && avail < 512) break;   // 输入不足, 等下一次 feed
//            size_t inNow = std::min(avail, (size_t)4096);
//            double target = (double)(p.fed.size() / 2) / (double)p.ratio;
//            size_t outNow = target >= (double)p.produced ? (size_t)target - p.produced : 0;
//            if (outNow > (maxFrames - produced)) outNow = maxFrames - produced;
//            if (outNow == 0) outNow = 1;   // 提前 1 帧 (总体比例由累计目标保持)
//
//            p.inL.resize(inNow); p.inR.resize(inNow);
//            for (size_t i = 0; i < inNow; i++) {
//                p.inL[i] = p.fed[(p.fedConsumed + i) * 2];
//                p.inR[i] = p.fed[(p.fedConsumed + i) * 2 + 1];
//            }
//            p.outL.resize(outNow); p.outR.resize(outNow);
//            const float* ins[2] = { p.inL.data(), p.inR.data() };
//            float* outs[2] = { p.outL.data(), p.outR.data() };
//            p.stretch.process(ins, (int)inNow, outs, (int)outNow);
//            p.fedConsumed += inNow;
//            p.produced += outNow;
//
//            for (size_t i = 0; i < outNow; i++) {
//                float l = p.outL[i] * kScale, r = p.outR[i] * kScale;
//                if (l > 32767.0f) l = 32767.0f;
//                if (l < -32768.0f) l = -32768.0f;
//                if (r > 32767.0f) r = 32767.0f;
//                if (r < -32768.0f) r = -32768.0f;
//                out[(produced + i) * 2] = (int16_t)std::llround(l);
//                out[(produced + i) * 2 + 1] = (int16_t)std::llround(r);
//            }
//            produced += outNow;
//        } else if (p.produced < p.outTarget) {
//            // 输入耗尽: flush 排空尾音
//            size_t n = p.outTarget - p.produced;
//            if (n > (maxFrames - produced)) n = maxFrames - produced;
//            if (n > 1024) n = 1024;
//            p.outL.resize(n); p.outR.resize(n);
//            float* outs[2] = { p.outL.data(), p.outR.data() };
//            p.stretch.flush(outs, (int)n, 0);
//            p.produced += n;
//            for (size_t i = 0; i < n; i++) {
//                float l = p.outL[i] * kScale, r = p.outR[i] * kScale;
//                if (l > 32767.0f) l = 32767.0f;
//                if (l < -32768.0f) l = -32768.0f;
//                if (r > 32767.0f) r = 32767.0f;
//                if (r < -32768.0f) r = -32768.0f;
//                out[(produced + i) * 2] = (int16_t)std::llround(l);
//                out[(produced + i) * 2 + 1] = (int16_t)std::llround(r);
//            }
//            produced += n;
//        } else {
//            break;  // 全部完成
//        }
//    }
//    return produced;
//}

size_t SignalsmithStream::render(int16_t* out, size_t maxFrames) {
    if (!out || maxFrames == 0) return 0;
    Impl& p = *imp;
    const float kScale = 32768.0f;
    size_t produced = 0;

    while (produced < maxFrames) {
        if (p.fedConsumed < p.fed.size() / 2) {
            // 还有输入待处理 (含静音补丁): 用 process 推进
            const size_t avail = p.fed.size() / 2 - p.fedConsumed;
            if (!p.finished && avail < 512) break;   // 输入不足, 等下一次 feed

            // 每次最多消费 4096 帧输入
            size_t inNow = std::min(avail, (size_t)4096);
            // 根据本批输入帧数和 ratio 计算应产生的输出帧数（四舍五入）
            size_t outNow = (size_t)((double)inNow / (double)p.ratio + 0.5);
            if (outNow > (maxFrames - produced)) {
                outNow = maxFrames - produced;
                // 根据输出需求反推输入帧数，确保输入足够
                inNow = std::min(avail, (size_t)((double)outNow * (double)p.ratio + 0.5));
                if (inNow == 0) break;
            }
            if (outNow == 0) break;

            p.inL.resize(inNow); p.inR.resize(inNow);
            for (size_t i = 0; i < inNow; i++) {
                p.inL[i] = p.fed[(p.fedConsumed + i) * 2];
                p.inR[i] = p.fed[(p.fedConsumed + i) * 2 + 1];
            }
            p.outL.resize(outNow); p.outR.resize(outNow);
            const float* ins[2] = { p.inL.data(), p.inR.data() };
            float* outs[2] = { p.outL.data(), p.outR.data() };
            p.stretch.process(ins, (int)inNow, outs, (int)outNow);
            p.fedConsumed += inNow;
            p.produced += outNow;

            for (size_t i = 0; i < outNow; i++) {
                float l = p.outL[i] * kScale, r = p.outR[i] * kScale;
                if (l > 32767.0f) l = 32767.0f;
                if (l < -32768.0f) l = -32768.0f;
                if (r > 32767.0f) r = 32767.0f;
                if (r < -32768.0f) r = -32768.0f;
                out[(produced + i) * 2] = (int16_t)std::llround(l);
                out[(produced + i) * 2 + 1] = (int16_t)std::llround(r);
            }
            produced += outNow;
        } else if (p.produced < p.outTarget) {
            // 输入耗尽: flush 排空尾音
            size_t n = p.outTarget - p.produced;
            if (n > (maxFrames - produced)) n = maxFrames - produced;
            if (n > 1024) n = 1024;
            p.outL.resize(n); p.outR.resize(n);
            float* outs[2] = { p.outL.data(), p.outR.data() };
            p.stretch.flush(outs, (int)n, 0);
            p.produced += n;
            for (size_t i = 0; i < n; i++) {
                float l = p.outL[i] * kScale, r = p.outR[i] * kScale;
                if (l > 32767.0f) l = 32767.0f;
                if (l < -32768.0f) l = -32768.0f;
                if (r > 32767.0f) r = 32767.0f;
                if (r < -32768.0f) r = -32768.0f;
                out[(produced + i) * 2] = (int16_t)std::llround(l);
                out[(produced + i) * 2 + 1] = (int16_t)std::llround(r);
            }
            produced += n;
        } else {
            break;  // 全部完成
        }
    }
    return produced;
}

size_t SignalsmithStream::producedFrames() const { return imp->produced; }
