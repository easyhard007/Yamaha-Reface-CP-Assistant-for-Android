#pragma once
#include <set>
#include <vector>
#include <algorithm>

/**
 * 自动延音踏板管理器 — 纯状态机 + 冲突检测
 * 不直接操作 MIDI 或音符集合，只返回 Action 给 AudioEngine 执行
 */
class AutoSustainManager {
public:
    enum Action { NONE = 0, CC64_ON = 1, CC64_OFF = 2, REHOLD = 3 };

    Action setEnabled(bool enabled);
    bool isEnabled() const { return mEnabled; }
    bool isBreaking() const { return mIsBreaking; }

    /// 音符按下 → 冲突检测。需要传入当前低音集合。
    /// 返回 CC64_OFF (break) 或 NONE
    Action onNoteOn(int note, const std::set<int>& lowNotes);

    /// 音符松开 → try_repress
    /// 返回 REHOLD (CC64=127 + 重挂 PedalHeldNotes) 或 NONE
    Action onNoteOff(int note);

    /// 关闭时重置状态
    void reset();

private:
    bool mEnabled = false;
    bool mIsBreaking = false;

    bool checkCollision(int newNote, const std::set<int>& lowNotes) const;
};
