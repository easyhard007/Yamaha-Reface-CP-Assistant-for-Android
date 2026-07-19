#include "TempoDetector.h"
#include <algorithm>
#include <android/log.h>

#define TAG "TempoDetector"
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

TempoDetector::TempoDetector() = default;

// ===== 小节切换检测 =====

bool TempoDetector::feedNoteOn(int pitch, int velocity, double timeMs) {
    // 记录所有音符事件 (图表用)
    mNoteEvents.push_back({pitch, velocity, timeMs});

    // 10 秒无输入 → 清空 BPM 列表
    if (mLastNoteTime > 0 && (timeMs - mLastNoteTime) > 6000.0) {
        mBpmList.clear();
        mMeasureTimestamps.clear();
        mAnomalyCount = 0;
        mPhaseOffset = 0.0;
    }
    mLastNoteTime = timeMs;

    if (pitch > mSplitPoint) return false;

    bool triggered = false;

    if (!mLowNotesForDetect.empty()) {
        // 转 sorted vector
        std::vector<int> sorted(mLowNotesForDetect.begin(), mLowNotesForDetect.end());
        std::sort(sorted.begin(), sorted.end());

        bool cond1 = pitch < sorted.back();                    // 低于最高音
        bool cond2 = pitch != sorted.front();                  // 不是最低音
        bool cond3 = sorted.size() > 1 ? (pitch != sorted[1]) : true; // 不是次低音

        triggered = (cond1 && cond2 && cond3);

        if (triggered) {
            mMeasureTimestamps.push_back(timeMs);

            if (mMeasureTimestamps.size() >= 2) {
                double interval = timeMs - mMeasureTimestamps[mMeasureTimestamps.size() - 2];
                double rawBpm = 240000.0 / interval; // 4 个四分音符 = 240000ms/BPM
                processNewBPM(rawBpm);
            } else {
                __android_log_print(ANDROID_LOG_INFO, TAG, "First measure change @ %.0fms", timeMs);
                mPhaseOffset = timeMs;
            }

            calculateStablePhaseOffset();

            mLowNotesForDetect.clear();
            mLowNotesForDetect.insert(pitch);
        } else {
            mLowNotesForDetect.insert(pitch);
        }
    } else {
        mLowNotesForDetect.insert(pitch);
    }

    return triggered;
}

// ===== BPM 折叠与修正 =====

double TempoDetector::foldToRange(double bpm) const {
    double v = bpm;
    while (v > BPM_MAX) v /= 2.0;
    while (v < BPM_MIN) v *= 2.0;
    return v;
}

double TempoDetector::getCorrectedBPM(double rawBpm, double reference) const {
    double standard = foldToRange(rawBpm);
    if (mBpmList.empty()) return standard;
    if (std::abs(standard - reference) <= TOLERANCE) return standard;

    // 3/4 经过和弦
    double test75 = foldToRange(rawBpm * 0.75);
    if (std::abs(test75 - reference) <= TOLERANCE) {
        __android_log_print(ANDROID_LOG_INFO, TAG, "3/4 passing chord: %.1f → %.1f", rawBpm, test75);
        return test75;
    }

    // 1/4 回归正拍
    double test25 = foldToRange(rawBpm * 0.25);
    if (std::abs(test25 - reference) <= TOLERANCE) {
        __android_log_print(ANDROID_LOG_INFO, TAG, "1/4 return to downbeat: %.1f → %.1f", rawBpm, test25);
        return test25;
    }

    return standard;
}

void TempoDetector::processNewBPM(double rawBpm) {
    double folded = getCorrectedBPM(rawBpm, mBestBPM);
    mBpmList.push_back(folded);
    if (mBpmList.size() > 10) mBpmList.erase(mBpmList.begin());

    if (mBpmList.size() > 1) {
        if (std::abs(folded - mBestBPM) > TOLERANCE) {
            mAnomalyCount++;
            __android_log_print(ANDROID_LOG_WARN, TAG, "Anomaly: %.1f (count=%d/3)", folded, mAnomalyCount);
        } else {
            mAnomalyCount = 0;
        }
    }

    if (mAnomalyCount >= ANOMALY_MAX) {
        __android_log_print(ANDROID_LOG_INFO, TAG, "Tempo shift! Clearing old data.");
        while (mBpmList.size() > 3) mBpmList.erase(mBpmList.begin());
        while (mMeasureTimestamps.size() > 3) mMeasureTimestamps.erase(mMeasureTimestamps.begin());
        mAnomalyCount = 0;
    }

    calculateOptimalBPM();
}

void TempoDetector::calculateOptimalBPM() {
    if (mBpmList.empty()) return;

    std::vector<double> bestCluster;
    for (size_t i = 0; i < mBpmList.size(); i++) {
        std::vector<double> currentCluster;
        for (size_t j = 0; j < mBpmList.size(); j++) {
            if (std::abs(mBpmList[i] - mBpmList[j]) <= TOLERANCE) {
                currentCluster.push_back(mBpmList[j]);
            }
        }
        if (currentCluster.size() > bestCluster.size()) {
            bestCluster = currentCluster;
        }
    }

    double sum = 0.0;
    for (double b : bestCluster) sum += b;
    mBestBPM = sum / (double)bestCluster.size();
}

// ===== 相位偏移 (圆周平均 + 排异) =====

void TempoDetector::calculateStablePhaseOffset() {
    if (mMeasureTimestamps.empty()) return;
    double measureDuration = 240000.0 / mBestBPM;

    size_t start = mMeasureTimestamps.size() > 16 ? mMeasureTimestamps.size() - 16 : 0;
    std::vector<double> recent(mMeasureTimestamps.begin() + start, mMeasureTimestamps.end());

    if (recent.size() < 3) {
        mPhaseOffset = recent.back();
        return;
    }

    double refAngle = fmod(mPhaseOffset, measureDuration) / measureDuration * 2.0 * M_PI;
    double sumSin = 0.0, sumCos = 0.0;
    int count = 0;

    for (double t : recent) {
        double angle = fmod(t, measureDuration) / measureDuration * 2.0 * M_PI;
        double diff = atan2(sin(angle - refAngle), cos(angle - refAngle));

        if (std::abs(diff) < M_PI / 4.0) {
            sumSin += sin(angle);
            sumCos += cos(angle);
            count++;
        }
    }

    if (count > 0) {
        double avgAngle = atan2(sumSin, sumCos);
        if (avgAngle < 0.0) avgAngle += 2.0 * M_PI;
        mPhaseOffset = (avgAngle / (2.0 * M_PI)) * measureDuration;
    } else {
        mPhaseOffset = recent.back();
    }
}

// ===== 清理旧音符 =====

void TempoDetector::pruneNoteEvents(double nowMs) {
    double cutoff = nowMs - 6000.0; // 6s 保留供 JS 5s 渐隐
    mNoteEvents.erase(
        std::remove_if(mNoteEvents.begin(), mNoteEvents.end(),
            [cutoff](const NoteEvent& e) { return e.timeMs < cutoff; }),
        mNoteEvents.end());
}
