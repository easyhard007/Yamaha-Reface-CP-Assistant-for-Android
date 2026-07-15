//
// Created by easyh on 2026/7/14.
//

#ifndef YAMAHA_REFACE_CP_ASSISTANT_STYLEPARSER_H
#define YAMAHA_REFACE_CP_ASSISTANT_STYLEPARSER_H

#pragma once
#include <vector>
#include <string>
#include <cstdint>
#include <fstream>
#include <iostream>

// 场景标记结构体
struct SceneMarker {
    std::string name;     // 场景名称 (如 "Main A", "Intro A")
    uint32_t absoluteTick; // 该场景在 MIDI 轨道中的绝对时间位置
};

// MIDI 事件结构体
struct MidiEvent {
    uint32_t absoluteTick; // 绝对 tick
    uint8_t  channel;      // MIDI 通道 (0-15)
    uint8_t  eventType;    // 0x90=NoteOn, 0x80=NoteOff, 0xC0=ProgramChange, 0xB0=CC
    uint8_t  data1;        // 音符号 / 控制器号 / 音色号
    uint8_t  data2;        // 力度 / 控制器值
};

class StyleParser {
private:
    std::vector<uint8_t> fileData;
    uint32_t resolution = 1920;
    uint32_t tempo = 500000;
    int timeSigNum = 4;   // 拍号分子 (默认 4/4)
    int timeSigDenom = 4;

    // 提取出的数据
    std::vector<SceneMarker> scenes;
    std::vector<MidiEvent>   events;
    std::vector<uint8_t>     casmData;

    // 每通道的 CC 状态 (由 SInt 段的 CC 事件更新)
    struct ChanBank { int msb = 0; int lsb = 0; int volume = 100; int pan = 64; int reverb = 0; int chorus = 0; };
    ChanBank channelState[16];

    // --- MIDI 解析辅助函数 ---
    uint32_t readBigEndian32(size_t index);
    uint16_t readBigEndian16(size_t index);
    uint32_t readVLQ(size_t& index); // 读取 MIDI 特有的变长数值 (Variable Length Quantity)

public:
    bool loadFromFile(const std::string& filePath);

    // 获取解析结果
    uint32_t getResolution() const { return resolution; }
    uint32_t getTempo() const { return tempo; }
    const std::vector<SceneMarker>& getScenes() const { return scenes; }
    const std::vector<MidiEvent>&   getEvents() const { return events; }
    const std::vector<uint8_t>&     getCasmData() const { return casmData; }

    // 获取通道在指定 tick 之前的 MSB/LSB (CC0/CC32)
    int getChannelMSB(int channel) const { return channelState[channel].msb; }
    int getChannelLSB(int channel) const { return channelState[channel].lsb; }
    int getChannelVolume(int channel) const { return channelState[channel].volume; }
    int getChannelPan(int channel) const { return channelState[channel].pan; }
    int getChannelReverb(int channel) const { return channelState[channel].reverb; }
    int getChannelChorus(int channel) const { return channelState[channel].chorus; }
    int getTimeSigNum() const { return timeSigNum; }
    int getTimeSigDenom() const { return timeSigDenom; }

    void printSummary() const;
};

#endif //YAMAHA_REFACE_CP_ASSISTANT_STYLEPARSER_H
