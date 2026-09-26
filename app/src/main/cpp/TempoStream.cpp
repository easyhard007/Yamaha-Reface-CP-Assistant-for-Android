#include "TempoStream.h"

#include "sbsms.h"

#include <cmath>
#include <cstdlib>

namespace {

constexpr int kProcessRate = 44100;  // 与 Audacity SBSMSBase 一致

}  // namespace

struct TempoStream::Impl {
    const int sampleRate;
    const float ratio;
    const double ratioA;  // 44.1k 输出帧 / 48k 输入帧
    const double ratioB;  // 48k 输出帧 / 44.1k 输入帧

    std::vector<float> inBuf;   // 48k float 交错 (feed 累积)
    bool finished = false;
    double aOutPos = 0.0;       // 输入重采样已产出 44.1k 帧数

    _sbsms_::SBSMSQuality quality{&_sbsms_::SBSMSQualityStandard};
    _sbsms_::Slide rateSlide{_sbsms_::SlideConstant, ratio, ratio, 0};
    _sbsms_::Slide pitchSlide{_sbsms_::SlideConstant, 1.0f, 1.0f, 0};

    class StreamIface;
    std::unique_ptr<_sbsms_::SBSMS> sbsms;
    std::unique_ptr<StreamIface> iface;
    bool sbsmsInit = false;

    std::vector<float> stretched;  // sbsms 产出 (44.1k float 交错)
    bool drained = false;
    double bOutPos = 0.0;          // 输出重采样已产出 48k 帧数
    size_t totalOut = 0;           // 预期总输出 (48k 帧) = 输入帧数 / ratio
    double margin0 = 0.0;          // 单次 read(512) 的最大输入消耗上限 (44.1k 帧)

    std::vector<float> readBlock;  // sbsms read 临时块 (512 帧 × 2)
    std::vector<float> renBlock;   // render 临时块

    double inputFed44k() const { return (double)(inBuf.size() / 2) * ratioA; }

    void ensureInit() {
        if (sbsmsInit) return;
        // 恒定比例拉伸: getStretch 与 t 无关, samplesToInput 仅用于时间归一化,
        // 传入一个足够大的上界即可 (真实总长度在 finishInput 时确定)
        iface = std::make_unique<StreamIface>(this, (_sbsms_::SampleCountType)(1LL << 30));
        sbsms = std::make_unique<_sbsms_::SBSMS>(2, &quality, true);
        sbsmsInit = true;
    }

    Impl(int sr, float r)
        : sampleRate(sr), ratio(r),
          ratioA((double)kProcessRate / (double)sr),
          ratioB((double)sr / (double)kProcessRate) {
        // 单次 read(512) 的最大输入消耗实测 ~22272 帧 (启动预填充 + 轨道再同步),
        // 余量不足会导致 sbsms 零填充 (输出数字静音段), 故取 1.5 倍安全系数
        margin0 = ((double)quality.getMaxPresamples() +
                   (double)quality.getFrameSize() * 2.0 +
                   512.0 * (double)ratio + 512.0) * 1.5;
        readBlock.resize(512 * 2);
    }
};

class TempoStream::Impl::StreamIface final : public _sbsms_::SBSMSInterfaceSliding {
public:
    StreamIface(TempoStream::Impl* p, _sbsms_::SampleCountType samplesToInput)
        : SBSMSInterfaceSliding(&p->rateSlide, &p->pitchSlide, false,
                                samplesToInput, 0, &p->quality),
          p(p) {}

    // sbsms 拉取 44.1k 输入帧: 从 48k inBuf 线性插值
    long samples(_sbsms_::audio* buf, long n) override {
        const size_t inAvail = p->inBuf.size() / 2;

        // 关键修改：如果本次请求无法被完整满足，且输入还没结束，
        // 直接返回 0，绝不返回部分数据。
        // 否则 sbsms 会用零填充请求中未满足的部分，把零当成真实输入
        // 送进相位声码器，输出端就会出现约 100ms 的静音/卡顿。
        if (!p->finished) {
            // 计算本次请求需要的最后一个输入帧位置（48k 空间）
            const double lastInPos = (p->aOutPos + (double)(n - 1)) / p->ratioA;
            const size_t lastIp = (size_t)lastInPos;
            if (lastIp + 1 >= inAvail) {
                return 0;  // aOutPos 不变，等下次 feed 后再来
            }
        }

        long produced = 0;
        for (long i = 0; i < n; i++) {
            const double inPos = (p->aOutPos + (double)produced) / p->ratioA;
            const size_t ip = (size_t)inPos;
            if (ip + 1 >= inAvail) {
                // 到这里说明 p->finished == true，且已经读到尾部
                if (p->finished && ip < inAvail) {
                    buf[i][0] = p->inBuf[ip * 2];
                    buf[i][1] = p->inBuf[ip * 2 + 1];
                    produced++;
                    continue;
                }
                break;  // 真正的输入结束
            }
            const float frac = (float)(inPos - (double)ip);
            buf[i][0] = p->inBuf[ip * 2] + (p->inBuf[ip * 2 + 2] - p->inBuf[ip * 2]) * frac;
            buf[i][1] = p->inBuf[ip * 2 + 1] + (p->inBuf[ip * 2 + 3] - p->inBuf[ip * 2 + 1]) * frac;
            produced++;
        }
        p->aOutPos += (double)produced;
        return produced;
    }

private:
    TempoStream::Impl* p;
};

TempoStream::TempoStream(int sampleRate, float ratio)
    : imp(std::make_unique<Impl>(sampleRate, ratio)) {}

TempoStream::~TempoStream() = default;

void TempoStream::feed(const int16_t* data, size_t frames) {
    if (!data || frames == 0 || imp->finished) return;
    const size_t base = imp->inBuf.size();
    imp->inBuf.resize(base + frames * 2);
    for (size_t i = 0; i < frames * 2; i++)
        imp->inBuf[base + i] = data[i] * (1.0f / 32768.0f);
}

void TempoStream::finishInput() {
    imp->finished = true;
    // 真实总输出帧数 = 输入帧数 / ratio (48k 帧)
    imp->totalOut = (size_t)((double)(imp->inBuf.size() / 2) / (double)imp->ratio + 0.5);
    imp->ensureInit();
}

bool TempoStream::inputFinished() const { return imp->finished; }

size_t TempoStream::render(int16_t* out, size_t maxFrames) {
    if (!out || maxFrames == 0) return 0;
    Impl& p = *imp;
    if (!p.sbsmsInit) p.ensureInit();
    if (!p.sbsmsInit) return 0;

    const float kScale = 32768.0f;
    size_t produced = 0;

    while (produced < maxFrames) {
        // 输出已到预期总量 → 停止 (防止 sbsms 在输入耗尽后零填充产生垃圾输出)
        if (p.finished && p.bOutPos >= (double)p.totalOut) break;

        // 输入未完成时: 确保剩余输入足够本轮回渲染所需 (含 read(512) 放大与预填充),
        // 防止 sbsms 因输入不足而零填充 (产生静音段);
        // 按安全余量限制本轮可产出的帧数, 让首次输出尽早出现
        if (!p.finished) {
            const double avail44 = p.inputFed44k() - p.aOutPos;
            const double maxSafe = (avail44 - p.margin0) / (double)p.ratio;
            if (maxSafe <= 0.0) break;
            if ((double)(maxFrames - produced) > maxSafe)
                maxFrames = produced + (size_t)maxSafe;
        }
        // 确保 stretched 提供下一输出帧及前瞻帧
        const double needIn = p.bOutPos / p.ratioB + 2.0;
        while ((double)(p.stretched.size() / 2) < needIn && !p.drained) {
            const long n = p.sbsms->read(p.iface.get(),
                reinterpret_cast<_sbsms_::audio*>(p.readBlock.data()), 512);
            if (n <= 0) {
                // 仅当输入真正结束时才标记排空; 否则是输入不足, 等待下次 feed
                if (p.finished) p.drained = true;
                break;
            }
            for (long i = 0; i < n; i++) {
                p.stretched.push_back(p.readBlock[i * 2]);
                p.stretched.push_back(p.readBlock[i * 2 + 1]);
            }
        }
        const size_t avail = p.stretched.size() / 2;
        const double inPos = p.bOutPos / p.ratioB;
        const size_t ip = (size_t)inPos;
        if (ip >= avail) break;
        size_t ip2 = ip + 1;
        if (ip2 >= avail) {
            if (!p.drained || !p.finished) break;
            ip2 = ip;  // 末帧钳位
        }
        const float frac = (float)(inPos - (double)ip);
        const float l = p.stretched[ip * 2] + (p.stretched[ip2 * 2] - p.stretched[ip * 2]) * frac;
        const float r = p.stretched[ip * 2 + 1] + (p.stretched[ip2 * 2 + 1] - p.stretched[ip * 2 + 1]) * frac;
        float lv = l * kScale, rv = r * kScale;
        if (lv > 32767.0f) lv = 32767.0f;
        if (lv < -32768.0f) lv = -32768.0f;
        if (rv > 32767.0f) rv = 32767.0f;
        if (rv < -32768.0f) rv = -32768.0f;
        out[produced * 2] = (int16_t)std::llround(lv);
        out[produced * 2 + 1] = (int16_t)std::llround(rv);
        produced++;
        p.bOutPos += 1.0;
    }
    return produced;
}

size_t TempoStream::producedFrames() const { return (size_t)imp->bOutPos; }
