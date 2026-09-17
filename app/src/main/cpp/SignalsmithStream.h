#pragma once

#include <vector>
#include <memory>
#include <cstdint>

/// 低延迟流式时间拉伸 (Signalsmith Stretch, 低质量小窗预设)
/// 用于 variation 尚未高质量渲染时的即时播放 (点击几乎立刻出声)。
/// 仅由单一渲染线程调用。
class SignalsmithStream {
public:
    SignalsmithStream(int sampleRate, float ratio);
    ~SignalsmithStream();

    /// 喂入原始 PCM (int16 交错立体声)
    void feed(const int16_t* data, size_t frames);

    /// 全部输入已喂完 (自动补输入延迟帧静音, 之后 render 排空尾音)
    void finishInput();
    bool inputFinished() const;

    /// 渲染至多 maxFrames 帧拉伸输出 (int16 交错立体声), 返回实际帧数
    size_t render(int16_t* out, size_t maxFrames);

    /// 已产出输出帧数
    size_t producedFrames() const;

private:
    struct Impl;
    std::unique_ptr<Impl> imp;
};
