#include "StyleParser.h"

// 辅助函数：读取大端 32 位整数 (MIDI 协议默认大端)
uint32_t StyleParser::readBigEndian32(size_t index) {
    if (index + 3 >= fileData.size()) return 0;
    return (fileData[index] << 24) | (fileData[index + 1] << 16) |
           (fileData[index + 2] << 8) | fileData[index + 3];
}

// 辅助函数：读取大端 16 位整数
uint16_t StyleParser::readBigEndian16(size_t index) {
    if (index + 1 >= fileData.size()) return 0;
    return (fileData[index] << 8) | fileData[index + 1];
}

// 辅助函数：读取 MIDI 变长数值 (Delta Time)
uint32_t StyleParser::readVLQ(size_t& index) {
    uint32_t value = 0;
    uint8_t byte;
    do {
        if (index >= fileData.size()) break;
        byte = fileData[index++];
        value = (value << 7) | (byte & 0x7F);
    } while (byte & 0x80); // 最高位为 1 说明还有下一个字节
    return value;
}

bool StyleParser::loadFromFile(const std::string& filePath) {
    // 1. 将整个文件读入内存
    std::ifstream file(filePath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        std::cerr << "Failed to open file: " << filePath << std::endl;
        return false;
    }
    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    fileData.resize(size);
    if (!file.read(reinterpret_cast<char*>(fileData.data()), size)) return false;

    scenes.clear();
    events.clear();
    casmData.clear();
    bool firstTempoFound = false;

    // 2. 开始遍历各个 Chunk 块
    size_t cursor = 0;
    while (cursor + 8 <= fileData.size()) {
        // 读取 4 字节的 Chunk ID 和 4 字节的长度
        std::string chunkId(reinterpret_cast<char*>(&fileData[cursor]), 4);
        uint32_t chunkLen = readBigEndian32(cursor + 4);
        cursor += 8;

        if (chunkId == "MThd") {
            // 解析 MIDI 文件头
            resolution = readBigEndian16(cursor + 4);
        }
        else if (chunkId == "MTrk") {
            // 解析音符轨道，提取 Marker 和 MIDI 事件
            size_t trackEnd = cursor + chunkLen;
            uint32_t absoluteTick = 0;
            uint8_t runningStatus = 0; // 记录前一个状态字节

            size_t tCursor = cursor;
            while (tCursor < trackEnd) {
                // 读取时间增量
                uint32_t deltaTime = readVLQ(tCursor);
                absoluteTick += deltaTime;

                uint8_t statusByte = fileData[tCursor];

                // 处理 MIDI 的 "Running Status" 机制 (省略状态字节)
                if (statusByte < 0x80) {
                    statusByte = runningStatus;
                } else {
                    runningStatus = statusByte;
                    tCursor++;
                }

                // --- Meta Event ---
                if (statusByte == 0xFF) {
                    uint8_t metaType = fileData[tCursor++];
                    uint32_t metaLen = readVLQ(tCursor);

                    if (metaType == 0x06) { // 0x06 代表 Marker (场景名)
                        std::string markerName(reinterpret_cast<char*>(&fileData[tCursor]), metaLen);
                        scenes.push_back({markerName, absoluteTick});
                    }
                    else if (metaType == 0x51 && metaLen == 3) { // Set Tempo
                        uint32_t newTempo = (fileData[tCursor] << 16) |
                                            (fileData[tCursor + 1] << 8) |
                                            fileData[tCursor + 2];
                        if (!firstTempoFound) { tempo = newTempo; firstTempoFound = true; }
                    }
                    else if (metaType == 0x58 && metaLen == 4) { // Time Signature
                        timeSigNum = fileData[tCursor];
                        timeSigDenom = 1 << fileData[tCursor + 1]; // 2^dd
                    }
                    tCursor += metaLen; // 跳过 meta 数据
                }
                // --- SysEx ---
                else if (statusByte == 0xF0 || statusByte == 0xF7) {
                    uint32_t sysLen = readVLQ(tCursor);
                    tCursor += sysLen;
                }
                // --- MIDI 通道事件 ---
                else {
                    uint8_t messageType = statusByte & 0xF0;
                    uint8_t channel = statusByte & 0x0F;

                    if (messageType == 0x80 || messageType == 0x90) {
                        // Note Off (0x80) or Note On (0x90)
                        uint8_t data1 = fileData[tCursor++];     // note
                        uint8_t data2 = fileData[tCursor++];     // velocity
                        // Note On with velocity 0 = Note Off
                        uint8_t evType = (messageType == 0x90 && data2 > 0) ? 0x90 : 0x80;
                        events.push_back({absoluteTick, channel, evType, data1, data2});
                    }
                    else if (messageType == 0xA0) {
                        // Poly Aftertouch (跳过)
                        tCursor += 2;
                    }
                    else if (messageType == 0xB0) {
                        // Control Change
                        uint8_t ccNum  = fileData[tCursor++];
                        uint8_t ccVal  = fileData[tCursor++];
                        events.push_back({absoluteTick, channel, 0xB0, ccNum, ccVal});
                        // 追踪每通道的 CC 状态
                        if (ccNum == 0)   channelState[channel].msb = ccVal;
                        if (ccNum == 32)  channelState[channel].lsb = ccVal;
                        if (ccNum == 7)   channelState[channel].volume = ccVal;
                        if (ccNum == 10)  channelState[channel].pan = ccVal;
                        if (ccNum == 91)  channelState[channel].reverb = ccVal;
                        if (ccNum == 93)  channelState[channel].chorus = ccVal;
                    }
                    else if (messageType == 0xC0) {
                        // Program Change
                        uint8_t prog = fileData[tCursor++];
                        events.push_back({absoluteTick, channel, 0xC0, prog, 0});
                    }
                    else if (messageType == 0xD0) {
                        // Channel Pressure (跳过)
                        tCursor += 1;
                    }
                    else if (messageType == 0xE0) {
                        // Pitch Bend (跳过)
                        tCursor += 2;
                    }
                }
            }
        }
        else if (chunkId == "CASM") {
            // 抓到雅马哈隐藏数据块！直接将二进制数据切片保存
            casmData.assign(fileData.begin() + cursor, fileData.begin() + cursor + chunkLen);
        }

        // 移向下一个块
        cursor += chunkLen;
    }

    return true;
}

// 打印测试结果
void StyleParser::printSummary() const {
    std::cout << "=== 雅马哈 Style 文件解析报告 ===" << std::endl;
    std::cout << "分辨率 (Ticks/Beat): " << resolution << std::endl;
    std::cout << "速度 (us/qn): " << tempo << " (" << (60000000.0 / tempo) << " BPM)" << std::endl;

    std::cout << "\n找到 " << scenes.size() << " 个伴奏场景 (Scenes):" << std::endl;
    for (const auto& scene : scenes) {
        std::cout << "  - [" << scene.absoluteTick << " ticks]\t" << scene.name << std::endl;
    }

    std::cout << "\n提取到 " << events.size() << " 个 MIDI 事件。" << std::endl;

    if (!casmData.empty()) {
        std::cout << "\n成功捕获 CASM 规则块，大小: " << casmData.size() << " 字节。" << std::endl;
        std::cout << "(CASM 包含伴奏引擎的移调公式)" << std::endl;
    } else {
        std::cout << "\n警告: 未找到 CASM 规则块，这可能是一个纯 MIDI 文件。" << std::endl;
    }
    std::cout << "===================================" << std::endl;
}
