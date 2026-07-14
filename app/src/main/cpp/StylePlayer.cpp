#include "StylePlayer.h"
#include <algorithm>
#include <chrono>
#include <sstream>
#include <cstring>
#include <cctype>
#include <set>

// ===== helpers =====
static std::string escapeJson(const std::string& s) {
    std::string r;
    for (char c : s) { if (c=='"') r+="\\\""; else if (c=='\\') r+="\\\\"; else r+=c; }
    return r;
}

bool StylePlayer::isMetaMarker(const std::string& name) {
    // 过滤 Yamaha sty 的元信息标记，不是场景
    return (name == "SFF1" || name == "SInt" || name == "Sint" ||
            name.find("SFF") == 0 || name.find("CASM") == 0);
}
bool StylePlayer::isFillScene(const std::string& name) {
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    return lower.find("fill") != std::string::npos;
}
bool StylePlayer::isEndingScene(const std::string& name) {
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    return lower.find("ending") != std::string::npos;
}

// ===== 在事件列表中寻找 note-off tick =====
uint32_t StylePlayer::findNoteOffTick(const std::vector<MidiEvent>& evts,
                                       size_t startIdx, uint8_t channel, uint8_t note) const {
    for (size_t i = startIdx; i < evts.size(); ++i) {
        const auto& e = evts[i];
        if (e.eventType == 0x80 && e.channel == channel && e.data1 == note)
            return e.absoluteTick;
    }
    return evts[startIdx].absoluteTick + (resolution / 4);
}

// 通道角色 → 默认 GM 乐器 (来自 JJazzLab AccType 定义)
// 当 MSB 为非标准值 (既非 0 也非鼓组) 时, PC 号不可信, 使用通道角色的默认乐器
struct ChannelDefault { int channel; int gmProg; };
static const ChannelDefault channelDefaults[] = {
    {10, 33}, // BASS     → Fingered Bass
    {11, 26}, // CHORD1   → Jazz Guitar
    {12, 0},  // CHORD2   → Acoustic Piano
    {13, 50}, // PAD      → Synth Strings 1
    {14, 62}, // PHRASE1  → Synth Brass 1
    {15, 2},  // PHRASE2  → E. Grand Piano
    // ch8/9 是鼓组, 已由 MSB>=126 处理
};
static const int channelDefaultsSize = sizeof(channelDefaults) / sizeof(channelDefaults[0]);

// ===== 构建场景 (过滤元标记) =====
void StylePlayer::buildScenes() {
    scenes.clear();
    resolution = parser.getResolution();
    tempo      = parser.getTempo();
    measureTicks = resolution * 4; // 默认 4/4 → 1小节 = 4拍

    const auto& markers = parser.getScenes();
    const auto& allEvents = parser.getEvents();

    if (markers.empty()) {
        uint32_t lastTick = allEvents.empty() ? 0 : allEvents.back().absoluteTick;
        SceneData sd;
        sd.name = "All"; sd.startTick = 0;
        sd.endTick = lastTick + resolution * 4;
        sd.durationTicks = sd.endTick - sd.startTick;
        for (size_t i = 0; i < allEvents.size(); ++i) {
            const auto& e = allEvents[i];
            if (e.eventType == 0x90) {
                uint32_t offTick = findNoteOffTick(allEvents, i + 1, e.channel, e.data1);
                sd.noteEvents.push_back({e.absoluteTick, e.channel, e.data1, e.data2,
                    offTick > e.absoluteTick ? offTick - e.absoluteTick : resolution / 4});
            } else {
                MidiEvent relEv = e; relEv.absoluteTick = e.absoluteTick;
                sd.controlEvents.push_back(relEv);
            }
        }
        scenes.push_back(std::move(sd));
        return;
    }

    for (size_t i = 0; i < markers.size(); ++i) {
        if (isMetaMarker(markers[i].name)) continue; // 跳过 SFF1, Sint 等

        SceneData sd;
        sd.name = markers[i].name;
        sd.startTick = markers[i].absoluteTick;

        // 找下一个有效场景的起始 tick
        uint32_t nextTick = 0xFFFFFFFF;
        for (size_t j = i + 1; j < markers.size(); ++j) {
            if (!isMetaMarker(markers[j].name)) { nextTick = markers[j].absoluteTick; break; }
        }
        if (nextTick == UINT32_MAX)
            nextTick = allEvents.empty() ? sd.startTick + resolution * 4
                                         : allEvents.back().absoluteTick + resolution * 4;
        sd.endTick = nextTick;
        sd.durationTicks = sd.endTick - sd.startTick;

        // 向前看一个16分音符, 捕获提前触发的起音音符 (如弦乐)
        uint32_t lookback = resolution / 4;
        uint32_t captureStart = (sd.startTick >= lookback) ? (sd.startTick - lookback) : 0;

        for (size_t j = 0; j < allEvents.size(); ++j) {
            const auto& e = allEvents[j];
            if (e.absoluteTick < captureStart) continue;
            if (e.absoluteTick >= sd.endTick) break;

            uint32_t relTick = (e.absoluteTick >= sd.startTick) ? (e.absoluteTick - sd.startTick) : 0;
            if (e.eventType == 0x90) {
                uint32_t offTick = findNoteOffTick(allEvents, j + 1, e.channel, e.data1);
                uint32_t dur = (offTick > e.absoluteTick) ? (offTick - e.absoluteTick) : (resolution / 4);
                sd.noteEvents.push_back({relTick, e.channel, e.data1, e.data2, dur});
            } else if (e.absoluteTick >= sd.startTick) {
                // Control event 只捕获标记之后的, 不回看
                MidiEvent relEv = e; relEv.absoluteTick = relTick;
                sd.controlEvents.push_back(relEv);
            }
        }
        scenes.push_back(std::move(sd));
    }
}

void StylePlayer::extractChannels() {
    channels.clear();
    int liveMSB[16] = {}, liveLSB[16] = {};
    int chanMSB[16] = {}, chanLSB[16] = {};
    int chanProg[16] = {}; bool hasPC[16] = {}, hasNote[16] = {};

    for (const auto& e : parser.getEvents()) {
        int ch = e.channel;
        if (e.eventType == 0xB0 && e.data1 == 0) {
            if (liveMSB[ch] != e.data2) hasPC[ch] = false;
            liveMSB[ch] = e.data2;
        }
        if (e.eventType == 0xB0 && e.data1 == 32) {
            if (liveLSB[ch] != e.data2) hasPC[ch] = false;
            liveLSB[ch] = e.data2;
        }
        if (e.eventType == 0xC0 && !hasPC[ch]) {
            hasPC[ch] = true;
            chanMSB[ch] = liveMSB[ch];
            chanLSB[ch] = liveLSB[ch];
            chanProg[ch] = e.data1;
        }
        if (e.eventType == 0x90 || e.eventType == 0x80) hasNote[ch] = true;
    }

    for (int ch = 0; ch < 16; ++ch) {
        if (!hasPC[ch] && !hasNote[ch]) continue;
        int msb = hasPC[ch] ? chanMSB[ch] : liveMSB[ch];
        bool isDrum = (msb >= 126);
        int bank = isDrum ? 128 : 0;
        int prog = hasPC[ch] ? chanProg[ch] : 0;
        if (isDrum && prog > 3) prog = 0;

        // 非 GM Bank (MSB>0 且非鼓): PC 号不可信, 用通道角色默认乐器
        if (!isDrum && msb > 0) {
            for (int i = 0; i < channelDefaultsSize; i++) {
                if (channelDefaults[i].channel == ch) {
                    prog = channelDefaults[i].gmProg;
                    break;
                }
            }
        }

        channels.push_back({ch, bank, prog, msb, hasPC[ch] ? chanLSB[ch] : liveLSB[ch],
                             parser.getChannelVolume(ch), parser.getChannelPan(ch),
                             parser.getChannelReverb(ch), parser.getChannelChorus(ch)});
    }
}

// ===== load / get info =====
StylePlayer::~StylePlayer() { stop(); }

bool StylePlayer::loadStyle(const std::string& filePath) {
    if (!parser.loadFromFile(filePath)) return false;
    overrides.clear(); // 清空旧覆盖, 使用 sty 原始乐器
    buildScenes();
    extractChannels();
    selectedScene.store(0);
    return true;
}

std::string StylePlayer::getScenesJson() const {
    std::ostringstream oss; oss << "[";
    for (size_t i = 0; i < scenes.size(); ++i) {
        if (i > 0) oss << ",";
        oss << "{\"name\":\"" << escapeJson(scenes[i].name) << "\""
            << ",\"tick\":" << scenes[i].startTick
            << ",\"notes\":" << scenes[i].noteEvents.size()
            << ",\"duration\":" << scenes[i].durationTicks << "}";
    }
    oss << "]"; return oss.str();
}

std::string StylePlayer::getChannelsJson() const {
    std::ostringstream oss; oss << "[";
    for (size_t i = 0; i < channels.size(); ++i) {
        if (i > 0) oss << ",";
        oss << "{\"channel\":" << channels[i].channel
            << ",\"bank\":" << channels[i].bank
            << ",\"program\":" << channels[i].program
            << ",\"msb\":" << channels[i].origMSB
            << ",\"lsb\":" << channels[i].origLSB
            << ",\"vol\":" << channels[i].volume
            << ",\"pan\":" << channels[i].pan
            << ",\"rev\":" << channels[i].reverb
            << ",\"cho\":" << channels[i].chorus << "}";
    }
    oss << "]"; return oss.str();
}

void StylePlayer::setChannelOverride(int channel, int bank, int program) {
    overrides[channel] = {bank, program};
}

void StylePlayer::toggleMute(int channel) {
    if (channel < 0 || channel > 15) return;
    muteMask.fetch_xor(1 << channel);
}
bool StylePlayer::isMuted(int channel) const {
    if (channel < 0 || channel > 15) return false;
    return (muteMask.load() & (1 << channel)) != 0;
}
uint16_t StylePlayer::getActiveChannels() const {
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    uint16_t mask = 0;
    for (int ch = 0; ch < 16; ch++) {
        if (nowMs - lastNoteMs[ch].load() < 300) mask |= (1 << ch);
    }
    return mask;
}

int StylePlayer::getPendingScene() const {
    int p = pendingScene.load();
    return (p >= 0 && p < (int)scenes.size()) ? p : -1;
}

// ===== 播放控制 =====
void StylePlayer::selectScene(int index) {
    if (index < 0 || index >= (int)scenes.size()) return;
    selectedScene.store(index);
    // 如果正在播放 → 请求切换
    if (playing.load()) {
        pendingScene.store(index);
    }
}

void StylePlayer::start(AudioEngine* audio, int target) {
    if (!audio || scenes.empty()) return;
    stop();

    int sel = selectedScene.load();
    if (sel < 0 || sel >= (int)scenes.size()) sel = 0;

    currentScene.store(sel);
    selectedScene.store(sel);
    pendingScene.store(-1);
    transition = TransitionType::None;
    needStop.store(false);
    playing.store(true);
    playbackThread = std::thread(&StylePlayer::playbackLoop, this, audio, target);
}

void StylePlayer::stop() {
    needStop.store(true);
    if (playbackThread.joinable()) playbackThread.join();
    playing.store(false);
    currentScene.store(-1);
    pendingScene.store(-1);
}

// ===== 播放循环 =====
void StylePlayer::playbackLoop(AudioEngine* audio, int target) {
    int chordRoot = 60; // 默认 C; 后续从外部注入
    uint32_t startRelTick = 0; // 放在循环外, 用于 fill seek 跨迭代

    while (!needStop.load()) {
        int curIdx = currentScene.load();
        if (curIdx < 0 || curIdx >= (int)scenes.size()) break;
        const auto& sd = scenes[curIdx];
        double usecPerTick = (double)tempo / (double)resolution;

        // 构建当前场景的播放队列 (按 tick 排序)
        struct QItem { uint32_t tick; int kind; size_t idx; uint32_t offTick; };
        std::vector<QItem> q;
        for (size_t i = 0; i < sd.noteEvents.size(); ++i)
            q.push_back({sd.noteEvents[i].startTick, 0, i, sd.noteEvents[i].startTick + sd.noteEvents[i].durationTicks});
        for (size_t i = 0; i < sd.controlEvents.size(); ++i)
            q.push_back({sd.controlEvents[i].absoluteTick, 2, i, 0});
        std::sort(q.begin(), q.end(), [](const QItem& a, const QItem& b) { return a.tick < b.tick; });

        // 乐器回退到 GM 兼容范围
        auto sendPC = [&](int ch, int bank, int prog) {
            if (bank == 128) {
                if (prog > 3) prog = 0;
            } else if (bank > 0) {
                bank = 0;
                // 非 GM Bank: PC 不直接对应 GM, 用通道角色默认
                for (int i = 0; i < channelDefaultsSize; i++) {
                    if (channelDefaults[i].channel == ch) {
                        prog = channelDefaults[i].gmProg;
                        break;
                    }
                }
            }
            audio->sendProgramChange(target, ch, bank, prog);
        };

        // 发送初始 program change (tick=0 的事件, 应用覆盖)
        {
            std::set<int> handled;
            for (const auto& e : sd.controlEvents) {
                if (e.absoluteTick == 0 && e.eventType == 0xC0) {
                    int b = 0, p = e.data1;
                    auto it = overrides.find(e.channel);
                    if (it != overrides.end()) { b = it->second.first; p = it->second.second; }
                    sendPC(e.channel, b, p);
                    handled.insert((int)e.channel);
                } else if (e.absoluteTick == 0 && e.eventType == 0xB0) {
                    audio->sendCC(target, e.channel, e.data1, e.data2);
                }
            }
            // 把所有被 override 但 sty 中没有 tick=0 program change 的通道也发一遍
            for (const auto& kv : overrides) {
                if (handled.find(kv.first) == handled.end()) {
                    sendPC(kv.first, kv.second.first, kv.second.second);
                }
            }
            // 把所有未被处理的 sty 原始通道也初始化 (避免残留上一轮的手动覆盖音色)
            for (const auto& ci : channels) {
                if (handled.find(ci.channel) == handled.end() &&
                    overrides.find(ci.channel) == overrides.end()) {
                    sendPC(ci.channel, ci.bank, ci.program);
                }
                // 发送每通道的 Pan/Reverb/Chorus (fluidLite 原生支持)
                audio->sendCC(target, ci.channel, 10, ci.pan);
                audio->sendCC(target, ci.channel, 91, ci.reverb);
                audio->sendCC(target, ci.channel, 93, ci.chorus);
                // CC7 不支持, 用 CC11 (Expression) 代替: sty 的 CC7 值映射到 CC11
                audio->sendCC(target, ci.channel, 11, ci.volume);
            }
        }

        struct PendingOff { uint32_t offTick; uint8_t ch; uint8_t note; };
        std::vector<PendingOff> pendingOffs;

        auto clearNotes = [&]() {
            for (auto& po : pendingOffs) audio->stopNote(target, po.ch, po.note);
            pendingOffs.clear();
        };

        // seeking 起始点 (fill 跳转时可能 >0)
        auto startTimeUs = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();

        // 遍历队列
        bool fillJump = false;
        for (size_t qi = 0; qi < q.size(); ++qi) {
            if (needStop.load()) { clearNotes(); return; }

            auto& item = q[qi];
            if (item.tick < startRelTick) continue;

            // ==== 检查 pending transition ====
            {
                int pending = pendingScene.load();
                if (pending >= 0 && pending < (int)scenes.size() && pending != currentScene.load()) {
                    const auto& tgt = scenes[pending];
                    if (isFillScene(tgt.name)) {
                        // Fill: 立即跳转, 消费 pending
                        pendingScene.store(-1);
                        uint32_t seekTick = (item.tick >= tgt.durationTicks)
                                            ? (item.tick % tgt.durationTicks) : item.tick;
                        clearNotes();
                        currentScene.store(pending);
                        transition = TransitionType::None;
                        startRelTick = seekTick;
                        fillJump = true;
                        break;
                    }
                    else if (isEndingScene(tgt.name)) {
                        transition = TransitionType::Ending;
                    } else {
                        transition = TransitionType::Normal;
                    }
                }
            }

            // ==== 小节边界检查 (Ending transition) ====
            if (transition == TransitionType::Ending) {
                int endIdx = pendingScene.load();
                if (endIdx < 0 || endIdx >= (int)scenes.size()) {
                    transition = TransitionType::None; // pending 已失效
                } else {
                    uint32_t curMeasure = item.tick / measureTicks;
                    if (curMeasure > 0 && (curMeasure % 2 == 0)) {
                        // 偶数小节边界 → 跳转
                        clearNotes();
                        currentScene.store(endIdx);
                        pendingScene.store(-1); // 消费
                        transition = TransitionType::None;
                        startRelTick = 0;
                        break;
                    }
                }
            }

            // ==== 等待目标时间 ====
            uint64_t targetUs = startTimeUs + (uint64_t)(item.tick * usecPerTick);
            while (true) {
                if (needStop.load()) { clearNotes(); return; }
                uint64_t nowUs = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                if (nowUs >= targetUs) break;
                uint64_t rem = targetUs - nowUs;
                if (rem > 1000) std::this_thread::sleep_for(std::chrono::microseconds(rem / 2));
                else std::this_thread::yield();
            }

            // ==== 清理到期的 note-off ====
            while (!pendingOffs.empty() && pendingOffs.front().offTick <= item.tick) {
                audio->stopNote(target, pendingOffs.front().ch, pendingOffs.front().note);
                pendingOffs.erase(pendingOffs.begin());
            }

            // ==== 执行事件 ====
            if (item.kind == 0) {
                const auto& ne = sd.noteEvents[item.idx];
                if (isMuted(ne.channel)) continue;
                int tpNote = transposeNote(ne.note, chordRoot, ne.channel);
                int vel = ne.velocity;
                if (ne.channel == 10) { vel = (int)(vel * 1.3f); if (vel > 127) vel = 127; }
                if (parser.getChannelMSB(ne.channel) >= 126)
                    tpNote = remapXGDrumToGM(tpNote);
                audio->playNote(target, ne.channel, tpNote, vel);
                auto now = std::chrono::steady_clock::now().time_since_epoch();
                lastNoteMs[ne.channel].store(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
                pendingOffs.push_back({item.offTick, ne.channel, static_cast<uint8_t>(tpNote)});
                std::sort(pendingOffs.begin(), pendingOffs.end(),
                    [](const PendingOff& a, const PendingOff& b) { return a.offTick < b.offTick; });
            } else if (item.kind == 2) {
                const auto& ce = sd.controlEvents[item.idx];
                if (ce.eventType == 0xC0) {
                    int b = 0, p = ce.data1;
                    auto it = overrides.find(ce.channel);
                    if (it != overrides.end()) { b = it->second.first; p = it->second.second; }
                    sendPC(ce.channel, b, p);
                } else if (ce.eventType == 0xB0) {
                    audio->sendCC(target, ce.channel, ce.data1, ce.data2);
                }
            }
        } // end for queue

        clearNotes();

        // ==== 场景结束: 处理 transition ====
        if (needStop.load()) return;
        {
            int pending = pendingScene.load();
            if (transition == TransitionType::Ending) {
                // Ending 只在偶数小节触发; 没触发 → 保留 pending, 循环本场景重试
                // pending 保留, 不清除
            } else if (transition == TransitionType::Normal &&
                       pending >= 0 && pending < (int)scenes.size()) {
                // Normal: 场景结束 → 切换并消费
                currentScene.store(pending);
                pendingScene.store(-1);
            } else if (pending >= 0 && pending < (int)scenes.size() &&
                       pending != currentScene.load()) {
                // 兜底: 如果有新 pending, 切换
                currentScene.store(pending);
                pendingScene.store(-1);
            }
            // 否则循环本场景
            transition = TransitionType::None;
        }
        if (!fillJump) startRelTick = 0; // fill 跳转保持 seekTick, 其他从头
    }
}

int StylePlayer::transposeNote(int note, int chordRoot, int channel) const {
    if (channel == 9) return note;
    int r = note + (chordRoot - 60);
    if (r < 0) r = 0; if (r > 127) r = 127;
    return r;
}

// XG 鼓组扩展音符 → GM 标准鼓音符降级映射
// 标准 GM 鼓 (35-81) 完全不变; 扩展区间的音符就近映射到相似 GM 音色
int StylePlayer::remapXGDrumToGM(int note) {
    if (note >= 35 && note <= 81) return note; // GM 区间无变化

    switch (note) {
        // 低音区 (13-34): XG 扩展鼓件 → GM 近似替代
        case 13: case 14: case 15: case 16: case 17: case 18:
            return 39; // Surdo/Scratch 等 → Hand Clap
        case 19: case 20: case 21: case 22: case 23: case 24:
            return 37; // Finger Snap / Click → Side Stick
        case 25: case 26: case 27: case 28: case 29: case 31:
            return 38; // Brush / Snare Roll / Snare Soft → Acoustic Snare
        case 30: return 75; // Castanets → Claves
        case 33: return 35; // Kick Soft → Acoustic Bass Drum
        case 34: return 37; // Rim Shot Open → Side Stick

        // 高音区 (82-84): XG 扩展 → GM 近似
        case 82: return 70; // Shaker → Maracas
        case 83: return 54; // Jingle Bell → Tambourine
        case 84: return 81; // Belltree → Open Triangle

        default: return note;
    }
}

// ===== 调试 dump =====
#include <fstream>
#include <iomanip>

static const char* noteNames[12] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
static std::string noteName(int midi) {
    if (midi < 0 || midi > 127) return "??";
    int oct = midi / 12 - 1;
    return std::string(noteNames[midi % 12]) + std::to_string(oct);
}
static std::string eventTypeName(uint8_t t) {
    switch (t) {
        case 0x90: return "NoteOn ";
        case 0x80: return "NoteOff";
        case 0xC0: return "ProgChg";
        case 0xB0: return "CC    ";
        default: return std::string("0x") + std::to_string(t);
    }
}

bool StylePlayer::dumpDebug(const std::string& styPath, const std::string& outputPath) {
    // 重新 parse 以确保数据新鲜
    StyleParser p;
    if (!p.loadFromFile(styPath)) return false;

    std::ofstream f(outputPath);
    if (!f.is_open()) return false;

    const auto& markers = p.getScenes();
    const auto& events = p.getEvents();

    f << "============================================================\n";
    f << "  STYLE DEBUG DUMP\n";
    f << "  File: " << styPath << "\n";
    f << "============================================================\n\n";

    // --- 头部 ---
    f << "--- HEADER ---\n";
    f << "Resolution (ticks/beat): " << p.getResolution() << "\n";
    f << "Tempo (us/qn): " << p.getTempo()
      << "  (" << (60000000.0 / p.getTempo()) << " BPM)\n";
    f << "Total events: " << events.size() << "\n";
    f << "Total markers: " << markers.size() << "\n";
    f << "CASM data: " << (p.getCasmData().empty() ? "none" : std::to_string(p.getCasmData().size()) + " bytes") << "\n\n";

    // --- Markers ---
    f << "--- MARKERS / SCENES ---\n";
    for (size_t i = 0; i < markers.size(); ++i) {
        f << "  [" << std::setw(6) << markers[i].absoluteTick << "]  "
          << markers[i].name;
        if (i + 1 < markers.size())
            f << "  (duration: " << (markers[i+1].absoluteTick - markers[i].absoluteTick) << " ticks)";
        f << "\n";
    }
    f << "\n";

    // --- 事件统计 ---
    f << "--- EVENT COUNTS ---\n";
    {
        std::map<int,int> noteOnByCh, noteOffByCh, pcByCh, ccByCh;
        for (const auto& e : events) {
            if (e.eventType == 0x90) noteOnByCh[e.channel]++;
            else if (e.eventType == 0x80) noteOffByCh[e.channel]++;
            else if (e.eventType == 0xC0) pcByCh[e.channel]++;
            else if (e.eventType == 0xB0) ccByCh[e.channel]++;
        }
        for (int ch = 0; ch < 16; ++ch) {
            if (noteOnByCh[ch] || pcByCh[ch]) {
                f << "  Ch " << std::setw(2) << ch
                  << ": NoteOn=" << std::setw(5) << noteOnByCh[ch]
                  << "  NoteOff=" << std::setw(5) << noteOffByCh[ch]
                  << "  ProgChg=" << std::setw(3) << pcByCh[ch]
                  << "  CC=" << std::setw(3) << ccByCh[ch];
                // 列出 program change 值
                bool first = true;
                for (const auto& e : events) {
                    if (e.eventType == 0xC0 && e.channel == ch) {
                        if (first) { f << "  programs=["; first = false; }
                        else f << ",";
                        f << (int)e.data1;
                    }
                }
                if (!first) f << "]";
                f << "\n";
            }
        }
    }
    f << "\n";

    // --- 按通道列出所有 Program Change ---
    f << "--- PROGRAM CHANGES (by channel, sorted by tick) ---\n";
    for (const auto& e : events) {
        if (e.eventType == 0xC0) {
            f << "  tick=" << std::setw(8) << e.absoluteTick
              << "  ch=" << std::setw(2) << (int)e.channel
              << "  prog=" << std::setw(4) << (int)e.data1 << "\n";
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

        f << "\n=== Scene [" << s << "] '" << markers[s].name
          << "'  ticks [" << startTick << ", " << endTick
          << ")  duration=" << (endTick - startTick) << " ===\n";

        // 该场景中的音符事件
        std::map<int, std::vector<std::pair<uint32_t,const MidiEvent*>>> notesByCh;
        for (const auto& e : events) {
            if (e.absoluteTick < startTick) continue;
            if (e.absoluteTick >= endTick) break;
            if (e.eventType == 0x90)
                notesByCh[e.channel].push_back({e.absoluteTick, &e});
        }

        for (int ch = 0; ch < 16; ++ch) {
            if (notesByCh[ch].empty()) continue;
            f << "  Ch " << std::setw(2) << ch << " (" << notesByCh[ch].size() << " notes):\n";
            int count = 0;
            for (const auto& p : notesByCh[ch]) {
                uint32_t relTick = p.first - startTick;
                f << "    tick=" << std::setw(6) << relTick
                  << "  note=" << std::setw(4) << noteName(p.second->data1)
                  << " (" << std::setw(3) << (int)p.second->data1 << ")"
                  << "  vel=" << std::setw(3) << (int)p.second->data2 << "\n";
                if (++count >= 30) {
                    f << "    ... (" << (notesByCh[ch].size() - 30) << " more notes)\n";
                    break;
                }
            }
        }
    }

    // --- 原始事件列表 (前500条) ---
    f << "\n--- RAW EVENTS (first 500 of " << events.size() << ") ---\n";
    f << "  tick      ch  type     data1  data2\n";
    int count = 0;
    for (const auto& e : events) {
        if (count++ >= 500) { f << "  ... truncated\n"; break; }
        f << "  " << std::setw(8) << e.absoluteTick
          << "  " << std::setw(2) << (int)e.channel
          << "  " << eventTypeName(e.eventType)
          << "  " << std::setw(5) << (int)e.data1
          << "  " << std::setw(5) << (int)e.data2;
        if (e.eventType == 0x90)
            f << "  (" << noteName(e.data1) << ")";
        f << "\n";
    }

    f << "\n============================================================\n";
    f << "  END OF DUMP\n";
    f << "============================================================\n";
    f.close();
    return true;
}
