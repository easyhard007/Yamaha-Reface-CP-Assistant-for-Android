#ifndef CHORD_DETECT_H
#define CHORD_DETECT_H

#include <vector>
#include <string>

struct DetectOptions {
    bool assumePerfectFifth = false;
};

// 核心检测函数：输入 MIDI 音符数组，返回按权重降序排列的候选和弦名称
std::vector<std::string> detect_chord(const std::vector<int>& midi_notes, DetectOptions options = {});

#endif // CHORD_DETECT_H