#pragma once

#include <vector>
#include <memory>
#include <cstdint>

/// 流式 sbsms 时间拉伸器 (有状态, 复刻 Audacity 管线:
/// 48k 输入 → 44.1kHz 重采样 → SBSMSQualityStandard → 重采样回 48k 输出)
///
/// 使用方式:
///   feed() 逐块喂入原始 PCM → render() 逐块产出拉伸后 PCM
///   (feed 全部完成后调用 finishInput(), render 直至产出 0)
///
/// 线程模型: 仅由单个渲染线程调用 (非线程安全)
class TempoStream {
public:
    TempoStream(int sampleRate, float ratio);
    ~TempoStream();

    /// 喂入原始 PCM (int16 交错立体声)
    void feed(const int16_t* data, size_t frames);

    /// 全部输入已喂完
    void finishInput();
    bool inputFinished() const;

    /// 渲染至多 maxFrames 帧拉伸输出到 out (int16 交错立体声), 返回实际帧数
    /// 输入未完成时会保留安全余量, 防止 sbsms 因输入耗尽而空转
    size_t render(int16_t* out, size_t maxFrames);

    /// 已产出输出帧数
    size_t producedFrames() const;

private:
    struct Impl;
    std::unique_ptr<Impl> imp;
};
