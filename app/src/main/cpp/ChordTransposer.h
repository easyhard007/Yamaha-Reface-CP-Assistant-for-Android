#pragma once
#include <vector>
#include <string>
#include <set>

class ChordTransposer {
public:
    ChordTransposer();

    /// 设置目标根音 (MIDI 音高, 默认 60=C4)
    void setChordRoot(int midiRoot) { mChordRoot = midiRoot; }
    int  getChordRoot() const { return mChordRoot; }
    const std::string& getChordType() const { return mChordType; }

    /// 传入和弦名 (如 "Em", "C5", "G major seventh", 或仅和弦类型如 "minor"), 解析类型并计算内音
    void setChordName(const std::string& name);

    /// 核心转调函数: 根据三规则返回转调后的 MIDI 音高
    /// channel: MIDI 通道 (0-15)
    /// note:    原始 MIDI 音高
    /// bank:    乐器 bank (0=GM, 128/120=鼓)
    /// program: 乐器 program
    int transpose(int channel, int note, int bank, int program) const;

    /// 获取当前和弦的内音音高集合 (MIDI 半音)
    const std::vector<int>& getChordTones() const { return mChordTones; }

private:
    int mChordRoot = 60;
    std::string mChordType = "major";
    std::vector<int> mIntervals;   // 相对根音的音程 (半音), 如 {0,4,7}
    std::vector<int> mChordTones;  // 内音 MIDI 音高集合 (多个八度)

    void parseChordName(const std::string& name); // 拆出根音和类型
    void computeTones();                          // 根据根音+音程展开到 MIDI 范围
    int  snapToNearest(int note) const;           // 就近映射到最近的内音
    bool isBassChannel(int bank, int program) const;
    bool isDrumChannel(int bank) const;
};
