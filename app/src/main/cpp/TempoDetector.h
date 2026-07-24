#pragma once
#include <vector>
#include <set>
#include <cstdint>
#include <cmath>

struct NoteEvent {
    int pitch;
    int velocity;
    double timeMs;
};

struct FencePost { double position; double weight; };

class TempoDetector {
public:
    TempoDetector();

    void setSplitPoint(int sp) { mSplitPoint = sp; }

    /// 喂入 MIDI NoteOn, 返回是否触发了小节切换
    bool feedNoteOn(int pitch, int velocity, double timeMs);

    /// 获取当前最佳 BPM
    double getBestBPM() const { return mBestBPM; }

    /// 获取相位偏移 (ms)
    double getPhaseOffset() const { return mPhaseOffset; }

    /// 获取近 10 秒的音符事件 (供图表)
    const std::vector<NoteEvent>& getNoteEvents() const { return mNoteEvents; }

    /// 获取小节时间戳 (调试用)
    const std::vector<double>& getMeasureTimestamps() const { return mMeasureTimestamps; }

    /// 获取 BPM 列表 (调试用)
    const std::vector<double>& getBpmList() const { return mBpmList; }

    int getAnomalyCount() const { return mAnomalyCount; }

    double getSyncOffset() const { return mSyncOffset; }
    void setSyncOffset(double v) { mSyncOffset = v; }

    double getSyncError() const { return mSyncError; }
    void setSyncError(double v) { mSyncError = v; }

    double getSyncMean() const { return mSyncMean; }
    void setSyncMean(double v) { mSyncMean = v; }

    /// 栅栏模板匹配: 联合搜索最优 BPM + Phase
    void calculateBestBpmAndPhase();

    /// 清理近 10 秒以外的旧音符事件
    void pruneNoteEvents(double nowMs);

private:
    int mSplitPoint = 52; // E3

    std::set<int> mLowNotesForDetect;
    std::vector<double> mMeasureTimestamps;
    std::vector<double> mBpmList;
    std::vector<NoteEvent> mNoteEvents;
    std::vector<FencePost> mFence;

    double mBestBPM = 75.0;
    double mPhaseOffset = 0.0;
    double mLastNoteTime = 0.0;
    double mSyncOffset = 0.0;
    double mSyncError = 0.0;
    double mSyncMean = 0.0;
    int mAnomalyCount = 0;

    static constexpr double BPM_MIN = 50.0;
    static constexpr double BPM_MAX = 130.0;
    static constexpr double TOLERANCE = 8.0;
    static constexpr int ANOMALY_MAX = 3;

    double foldToRange(double bpm) const;
    double getCorrectedBPM(double rawBpm, double reference) const;
    void processNewBPM(double rawBpm);
    void calculateOptimalBPM();
    void calculateStablePhaseOffset();
};
