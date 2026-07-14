// sty_dump.cpp — 独立命令行工具，解析并 dump Yamaha .sty 文件
// 编译 (Windows):  g++ -std=c++17 -O2 sty_dump.cpp StyleParser.cpp -o sty_dump.exe
// 编译 (macOS/Linux): g++ -std=c++17 -O2 sty_dump.cpp StyleParser.cpp -o sty_dump

#include "StyleParser.h"
#include <iostream>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>

static const char* noteNames[12] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
static std::string noteName(int midi) {
    if (midi < 0 || midi > 127) return "??";
    int oct = midi / 12 - 1;
    return std::string(noteNames[midi % 12]) + std::to_string(oct);
}
static const char* evType(uint8_t t) {
    switch (t) {
        case 0x90: return "NoteOn ";
        case 0x80: return "NoteOff";
        case 0xC0: return "ProgChg";
        case 0xB0: return "CC    ";
        default: return "Other  ";
    }
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: sty_dump <file.sty> [output.txt]\n";
        std::cerr << "  If output.txt is omitted, prints to stdout.\n";
        return 1;
    }
    std::string styPath = argv[1];
    std::ostream* outPtr = &std::cout;
    std::ofstream fileOut;
    if (argc >= 3) {
        fileOut.open(argv[2]);
        if (!fileOut.is_open()) { std::cerr << "Cannot open output file\n"; return 1; }
        outPtr = &fileOut;
    }
    std::ostream& f = *outPtr;

    StyleParser p;
    if (!p.loadFromFile(styPath)) {
        std::cerr << "Failed to load: " << styPath << "\n";
        return 1;
    }

    const auto& markers = p.getScenes();
    const auto& events  = p.getEvents();

    f << "============================================================\n";
    f << "  STYLE DEBUG DUMP\n";
    f << "  File: " << styPath << "\n";
    f << "============================================================\n\n";

    f << "--- HEADER ---\n";
    f << "Resolution (ticks/beat): " << p.getResolution() << "\n";
    f << "Tempo (us/qn): " << p.getTempo()
      << "  (" << (60000000.0 / p.getTempo()) << " BPM)\n";
    f << "Total events: " << events.size() << "\n";
    f << "Total markers: " << markers.size() << "\n";
    f << "CASM data: " << (p.getCasmData().empty() ? "none" : std::to_string(p.getCasmData().size()) + " bytes") << "\n\n";

    f << "--- MARKERS / SCENES ---\n";
    for (size_t i = 0; i < markers.size(); ++i) {
        f << "  [" << std::setw(7) << markers[i].absoluteTick << "]  " << markers[i].name;
        if (i + 1 < markers.size())
            f << "  (duration: " << (markers[i+1].absoluteTick - markers[i].absoluteTick) << " ticks)";
        f << "\n";
    }
    f << "\n";

    // --- 事件统计 ---
    f << "--- CHANNEL SUMMARY ---\n";
    {
        std::map<int,int> noteOn, noteOff, pc, cc;
        for (const auto& e : events) {
            if (e.eventType == 0x90) noteOn[e.channel]++;
            else if (e.eventType == 0x80) noteOff[e.channel]++;
            else if (e.eventType == 0xC0) pc[e.channel]++;
            else if (e.eventType == 0xB0) cc[e.channel]++;
        }
        for (int ch = 0; ch < 16; ++ch) {
            if (noteOn[ch] > 0 || pc[ch] > 0) {
                int msb = p.getChannelMSB(ch);
                int lsb = p.getChannelLSB(ch);
                bool isDrum = (msb >= 126);
                f << "  Ch " << std::setw(2) << ch
                  << "  Notes=" << std::setw(5) << noteOn[ch]
                  << "  ProgChg=" << std::setw(2) << pc[ch]
                  << "  CC=" << std::setw(3) << cc[ch]
                  << "  MSB=" << std::setw(3) << msb << " LSB=" << std::setw(3) << lsb
                  << (isDrum ? " [DRUM]" : "")
                  << "  progs=[";
                bool first = true;
                for (const auto& e : events)
                    if (e.eventType == 0xC0 && e.channel == ch) {
                        if (!first) f << ","; first = false;
                        f << (int)e.data1;
                    }
                f << "]\n";
            }
        }
    }
    f << "\n";

    // --- Program Change 列表 ---
    f << "--- ALL PROGRAM CHANGES (sorted by tick) ---\n";
    for (const auto& e : events) {
        if (e.eventType == 0xC0) {
            f << "  tick=" << std::setw(8) << e.absoluteTick
              << "  ch=" << std::setw(2) << (int)e.channel
              << "  prog=" << (int)e.data1 << "\n";
        }
    }
    f << "\n";

    // --- 按场景列出音符 ---
    f << "--- SCENE BREAKDOWN ---\n";
    for (size_t s = 0; s < markers.size(); ++s) {
        uint32_t startTick = markers[s].absoluteTick;
        uint32_t endTick = (s + 1 < markers.size()) ? markers[s+1].absoluteTick
                           : (events.empty() ? startTick + p.getResolution() * 4
                                             : events.back().absoluteTick + p.getResolution() * 4);
        f << "\n=== Scene [" << s << "] '" << markers[s].name << "'"
          << "  ticks [" << startTick << "," << endTick
          << ")  duration=" << (endTick - startTick) << " ===\n";

        std::map<int, std::vector<const MidiEvent*>> notesByCh;
        for (const auto& e : events) {
            if (e.absoluteTick < startTick) continue;
            if (e.absoluteTick >= endTick) break;
            if (e.eventType == 0x90) notesByCh[e.channel].push_back(&e);
        }
        for (int ch = 0; ch < 16; ++ch) {
            if (notesByCh[ch].empty()) continue;
            f << "  Ch " << std::setw(2) << ch << " (" << notesByCh[ch].size() << " notes):\n";
            int cnt = 0;
            for (auto pe : notesByCh[ch]) {
                f << "    relTick=" << std::setw(6) << (pe->absoluteTick - startTick)
                  << "  " << noteName(pe->data1) << "  vel=" << (int)pe->data2 << "\n";
                if (++cnt >= 40) {
                    f << "    ... (" << (notesByCh[ch].size() - 40) << " more)\n";
                    break;
                }
            }
        }
    }

    // --- 原始事件 (前 500 条) ---
    f << "\n--- RAW EVENTS (first 500 of " << events.size() << ") ---\n";
    int cnt = 0;
    for (const auto& e : events) {
        if (cnt++ >= 500) { f << "  ... truncated\n"; break; }
        f << "  " << std::setw(8) << e.absoluteTick
          << "  ch" << std::setw(2) << (int)e.channel
          << "  " << evType(e.eventType)
          << "  d1=" << std::setw(4) << (int)e.data1
          << "  d2=" << std::setw(4) << (int)e.data2;
        if (e.eventType == 0x90)
            f << "  (" << noteName(e.data1) << ")";
        f << "\n";
    }

    f << "\n============================================================\n";
    f << "  END OF DUMP\n";
    f << "============================================================\n";

    if (fileOut.is_open()) fileOut.close();
    return 0;
}
