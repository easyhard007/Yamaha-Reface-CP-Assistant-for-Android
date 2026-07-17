#pragma once

#include "StyleParser.h"
#include "AudioEngine.h"
#include "ChordTransposer.h"
#include "Humanizer.h"
#include <vector>
#include <string>
#include <thread>
#include <atomic>
#include <cstdint>
#include <map>

struct PlayEvent {
    uint32_t startTick;
    uint8_t  channel;
    uint8_t  note;
    uint8_t  velocity;
    uint32_t durationTicks;
    uint32_t hVelOff; // 人性化力度偏移 + 128 (临存)
};

struct SceneData {
    std::string name;
    uint32_t startTick;
    uint32_t endTick;
    uint32_t durationTicks; // endTick - startTick, for modulo
    std::vector<PlayEvent> noteEvents;
    std::vector<MidiEvent> controlEvents;
};

struct ChannelInst {
    int channel;
    int bank;     // 回退后的有效 bank (0=GM 旋律, 128=鼓)
    int program;  // 回退后的有效 program
    int origMSB;  // sty 原始 MSB (调试用)
    int origLSB;  // sty 原始 LSB (调试用)
    int volume;   // CC7 (0-127)
    int pan;      // CC10 (0-127, 64=center)
    int reverb;   // CC91
    int chorus;   // CC93
};

enum class TransitionType { None, Fill, Ending, Normal };

class StylePlayer {
public:
    StylePlayer() = default;
    ~StylePlayer();

    bool loadStyle(const std::string& filePath);

    std::string getScenesJson() const;
    std::string getChannelsJson() const;
    void setChannelOverride(int channel, int bank, int program);

    // Mute
    void toggleMute(int channel);
    bool isMuted(int channel) const;
    void mutePianoChannels(); // 默认静音所有钢琴/电钢通道

    // Activity LED: 返回最近 300ms 内有 note-on 的通道 bitmask
    uint16_t getActiveChannels() const;

    // --- 播放控制 ---
    void selectScene(int index);    // 选中场景 (播放中=请求切换)
    void start(AudioEngine* audio, int target, int beatIndex = 0);
    void stop();                    // 停止播放
    void setChordRoot(int root, const std::string& chordName);
    void setHumanize(float timing, float velocity);
    float getHumanizeTiming() const { return humanizer.timingRandomness; }
    float getHumanizeVelocity() const { return humanizer.velocityRandomness; }
    std::string getChordTonesString() const;
    std::string getDebugInfo() const;

    /// 调试: 将 sty 的全部信息 dump 到文本文件
    bool dumpDebug(const std::string& styPath, const std::string& outputPath);

    int  getCurrentScene() const { return currentScene.load(); }
    int  getPendingScene() const;   // -1 = 无 pending
    double getTempoBPM() const { return 60000000.0 / tempo; }
    void   setTempoBPM(double bpm); // 动态更新播放速度
    int getTimeSigNum() const { return timeSigNum; }
    int getCurrentBeat() const { return currentBeat.load(); }
    bool isPlaying() const { return playing.load(); }

private:
    StyleParser parser;
    std::vector<SceneData> scenes;
    std::vector<ChannelInst> channels;
    std::map<int, std::pair<int,int>> overrides;

    uint32_t resolution = 1920;
    uint32_t tempo = 800000; // 75 BPM
    int      measureTicks = 0;
    int      timeSigNum = 4;
    int      beatTicks = 0;

    std::thread   playbackThread;
    std::atomic<bool> playing{false};
    std::atomic<bool> needStop{false};
public:
    std::atomic<bool> beatZeroSignal{false}; // 节拍器线程置 true, 播放循环消费
private:
    std::atomic<int>  currentScene{-1};
    std::atomic<uint16_t> muteMask{0};
    std::atomic<int64_t> lastNoteMs[16]{};
    std::atomic<int> currentBeat{0};
    ChordTransposer chordTransposer;
    Humanizer humanizer;
    std::atomic<bool> chordRetrigger{false};
    std::mutex retriggerMutex;
    uint32_t fillShiftTicks = 0;
    uint32_t fillEndTick = 0;    // Fill 强制结束 tick
    uint32_t lastFillShift = 0;
    int      lastTimeOff = 0;   // timeOffset 调试值               // 保护 retrigger 状态
    std::atomic<int>  pendingScene{-1};
    std::atomic<int>  selectedScene{0};

    // pending transition type (只由 playback thread 写)
    TransitionType   transition = TransitionType::None;

    void buildScenes();
    void extractChannels();
    int  transposeNote(int note, int chordRoot, int channel) const;
    static int remapXGDrumToGM(int note);
    void playbackLoop(AudioEngine* audio, int target, uint32_t startRelTick);

    uint32_t findNoteOffTick(const std::vector<MidiEvent>& evts,
                             size_t startIdx, uint8_t channel, uint8_t note) const;

    static bool isFillScene(const std::string& name);
    static bool isEndingScene(const std::string& name);
    static bool isIntroScene(const std::string& name);
    static bool isMetaMarker(const std::string& name);
};
