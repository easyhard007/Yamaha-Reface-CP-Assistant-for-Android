#include "BassAssist.h"
#include "AudioEngine.h"
#include "RhythmAudioEngine.h"
#include <cmath>
#include <thread>
#include <chrono>
#include <android/log.h>

// ---- 音符名 → MIDI 音高 (A1=33 ~ #G2=44) ----
static int noteNameToMidi(const std::string& name) {
    if (name == "A")  return 33;
    if (name == "A#" || name == "Bb") return 34;
    if (name == "B")  return 35;
    if (name == "C")  return 36;
    if (name == "C#" || name == "Db") return 37;
    if (name == "D")  return 38;
    if (name == "D#" || name == "Eb") return 39;
    if (name == "E")  return 40;
    if (name == "F")  return 41;
    if (name == "F#" || name == "Gb") return 42;
    if (name == "G")  return 43;
    if (name == "G#" || name == "Ab") return 44;
    return -1;
}

// ---- 和弦 → 贝斯根音 MIDI 音高, 无和弦返回 -1 ----
int BassAssist::getBassPitchFromChord(const std::string& chord) {
    if (chord.empty() || chord == "-") return -1;

    std::string bassName;
    size_t slashPos = chord.find('/');
    if (slashPos != std::string::npos && slashPos + 1 < chord.size()) {
        bassName = chord.substr(slashPos + 1);
        if (bassName.size() > 1 && (bassName[1] == '#' || bassName[1] == 'b'))
            bassName = bassName.substr(0, 2);
        else
            bassName = bassName.substr(0, 1);
    } else {
        bassName = chord.substr(0, 1);
        if (chord.size() > 1 && (chord[1] == '#' || chord[1] == 'b'))
            bassName += chord[1];
    }

    return noteNameToMidi(bassName);
}

// ---- 音高 class → MIDI A1-#G2 ----
int BassAssist::pcToMidiA1_Gs2(int pc) {
    // A=9→33, B=11→35, C=0→36, ..., G#=8→44
    if (pc >= 9) return 33 + (pc - 9);
    return 36 + pc;
}

// ---- 能量 → 力度系数 ----
// e≤0.2→0, 0.2<e<0.6→线性0→1, e≥0.6→1
static float energyToVelFactor(float e) {
    if (e <= 0.2f) return 0.0f;
    if (e >= 0.6f) return 1.0f;
    return (e - 0.2f) / 0.4f;
}

// ---- 从 chordNotes 按概率选音 (概率随能量线性插值) ----
// e=0 → {100%, 0%} / {100%, 0%, 0%}
// e=1 → {60%, 40%} / {60%, 10%, 30%}
std::vector<int> BassAssist::pickNotesFromChord(const std::vector<int>& notes, float energy) {
    std::vector<int> result;
    if (notes.empty()) return result;
    float e = energy; if (e < 0) e = 0; if (e > 1) e = 1;
    if (notes.size() == 1) {
        result.push_back(pcToMidiA1_Gs2(notes[0]));
    } else if (notes.size() == 2) {
        float p1 = 1.0f - 0.4f * e;  // e=0→1.0, e=1→0.6
        int idx = ((rand() % 10000) / 10000.0f) < p1 ? 0 : 1;
        result.push_back(pcToMidiA1_Gs2(notes[idx]));
    } else {
        float p1 = 1.0f - 0.4f * e;  // e=0→1.0, e=1→0.6
        float p2 = 0.0f + 0.1f * e;  // e=0→0.0, e=1→0.1
        float r = (rand() % 10000) / 10000.0f;
        if (r < p1) result.push_back(pcToMidiA1_Gs2(notes[0]));
        else if (r < p1 + p2) result.push_back(pcToMidiA1_Gs2(notes[1]));
        else result.push_back(pcToMidiA1_Gs2(notes[2]));
    }
    return result;
}

// ===== 弹奏与制音 =====

void BassAssist::playBass(int pitch, int vel, double nowMs, AudioEngine& audio) {
    mDampGeneration++;
    audio.enqueueAllSoundsOff(2);
    audio.enqueueNoteOn(2, 0, pitch, vel);
    mBassNoteOn = pitch;
    mBassNoteTime = nowMs;
    mLastBassVel = vel;
}

void BassAssist::dampBass(double nowMs, double bpm, const std::vector<int>& chordNotes,
                           float energy, int triggerVelocity, AudioEngine& audio) {
    if (mBassNoteOn < 0) return;
    int dampPitch = mBassNoteOn;
    int gen = ++mDampGeneration;
    double measureMs = 240000.0 / bpm;
    int dampVelocity = (int)std::lround((double)triggerVelocity * 0.70);
    if (dampVelocity < 1) dampVelocity = 1;
    if (dampVelocity > 127) dampVelocity = 127;

    audio.enqueueNoteOff(2, 0, dampPitch);
    audio.enqueueNoteOn(2, 0, dampPitch, dampVelocity);
    mBassNoteOn = -1;

    AudioEngine* pAudio = &audio;
    std::atomic<int>* pGen = &mDampGeneration;
    std::vector<int> capturedNotes = chordNotes;
    float capturedEnergy = energy;

    // 50ms 后切断制音
    std::thread([pAudio, dampPitch, gen, pGen]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (pGen->load() != gen) return;
        pAudio->enqueueNoteOff(2, 0, dampPitch);
    }).detach();

    // measure/8 后重新触发 (用 chordNotes 选音)
    int* pBassNoteOn = &mBassNoteOn;
    double* pBassNoteTime = &mBassNoteTime;
    int* pLastBassVel = &mLastBassVel;
    std::thread([pAudio, gen, pGen, measureMs, capturedNotes, capturedEnergy,
                 pBassNoteOn, pBassNoteTime, pLastBassVel]() {
        std::this_thread::sleep_for(std::chrono::milliseconds((int64_t)(measureMs / 8.0)));
        if (pGen->load() != gen) return;
        auto picks = pickNotesFromChord(capturedNotes, capturedEnergy);
        if (picks.empty()) return;
        pAudio->enqueueAllSoundsOff(2);
        pAudio->enqueueNoteOn(2, 0, picks[0], 100);
        *pBassNoteOn = picks[0];
        *pLastBassVel = 100;
        auto now = std::chrono::steady_clock::now().time_since_epoch();
        *pBassNoteTime = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    }).detach();
}

// ===== 主入口 =====

void BassAssist::processStep(int step, int bassVel, int toneVel, double nowMs, double bpm,
                              const std::string& chordStr,
                              const std::vector<int>& chordNotes,
                              float energy,
                              AudioEngine& audio, RhythmAudioEngine* rhythmEng) {
    // Cajon Tone（军鼓）力度>64 → 制音；短促制音音符使用原鼓力度的 70%。
    if (toneVel > 64) dampBass(nowMs, bpm, chordNotes, energy, toneVel, audio);

    // 从和弦解析贝斯根音 (step 0 用)
    int bassPitch = getBassPitchFromChord(chordStr);

    // 无和弦 → 全部静音
    if (bassPitch < 0) {
        if (mBassNoteOn >= 0) {
            audio.enqueueAllSoundsOff(2);
            mBassNoteOn = -1;
        }
        mPrevBassPitch = -1;
        return;
    }

    if (!mEnabled.load()) {
        mPrevBassPitch = bassPitch;
        return;
    }

    // 场景 1: Cajon bass 触发 (>45) → 弹奏贝斯
    if (bassVel > 45) {
        float vf = energyToVelFactor(energy);
        if (vf > 0.0f) {
            int vel = (int)(100.0f * vf);
            if (step == 0) {
                vel = (int)(vel * 1.2f);  // step 0 力度加成
                if (vel > 127) vel = 127;
                __android_log_print(ANDROID_LOG_INFO, "BassAssist",
                    "trigger step=%d chord=%s pitch=%d vel=%d e=%.2f", step, chordStr.c_str(), bassPitch, vel, energy);
                playBass(bassPitch, vel, nowMs, audio);
            } else {
                auto picks = pickNotesFromChord(chordNotes, energy);
                if (!picks.empty()) playBass(picks[0], vel, nowMs, audio);
            }
        }
    }

    // 场景 2: 和弦根音变化
    if (mPrevBassPitch > 0 && bassPitch > 0 && bassPitch != mPrevBassPitch && mBassNoteOn >= 0) {
        float vf = energyToVelFactor(energy);
        if (vf > 0.0f) {
            double elapsed = nowMs - mBassNoteTime;
            if (elapsed < 3200.0) {
                int vel = (int)((3200.0 - elapsed) / 3200.0 * mLastBassVel * vf);
                if (vel > 0) playBass(bassPitch, vel, nowMs, audio);
            }
        }
    }
    mPrevBassPitch = bassPitch;
    mPrevBassTime = nowMs;

    // 10 秒超时自动 note-off
    if (mBassNoteOn >= 0 && (nowMs - mBassNoteTime) > 10000.0) {
        audio.enqueueNoteOff(2, 0, mBassNoteOn);
        mBassNoteOn = -1;
    }
}
