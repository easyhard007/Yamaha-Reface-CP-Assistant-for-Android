#include <iostream>
#include <vector>
#include <string>
#include <algorithm>
#include <windows.h>
#include <mmsystem.h>
#include "chord_detect.h"

#pragma comment(lib, "winmm.lib")

std::vector<int> active_notes; 
CRITICAL_SECTION cs;           

std::string midi_to_name(int note) {
    static const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    int pc = note % 12;
    int octave = (note / 12) - 1;
    return std::string(names[pc]) + std::to_string(octave);
}

void CALLBACK MidiInProc(
    HMIDIIN   hMidiIn,
    UINT      wMsg,
    DWORD_PTR dwInstance,
    DWORD_PTR dwParam1,
    DWORD_PTR dwParam2
) {
    if (wMsg == MIM_DATA) {
        DWORD msg = dwParam1;
        int status = msg & 0xFF;
        int cmd = status & 0xF0;
        int note = (msg >> 8) & 0xFF;
        int velocity = (msg >> 16) & 0xFF;

        bool isNoteOn = (cmd == 0x90 && velocity > 0);
        bool isNoteOff = (cmd == 0x80) || (cmd == 0x90 && velocity == 0);

        if (isNoteOn || isNoteOff) {
            EnterCriticalSection(&cs); 

            if (isNoteOn) {
                if (std::find(active_notes.begin(), active_notes.end(), note) == active_notes.end()) {
                    active_notes.push_back(note);
                    std::sort(active_notes.begin(), active_notes.end());
                }
            } 
            else if (isNoteOff) {
                active_notes.erase(
                    std::remove(active_notes.begin(), active_notes.end(), note), 
                    active_notes.end()
                );
            }

            std::string notes_str = "Notes: [ ";
            for (int n : active_notes) {
                notes_str += midi_to_name(n) + " ";
            }
            notes_str += "]  ->  ";

            DetectOptions options;
            options.assumePerfectFifth = true;
            std::vector<std::string> chords = detect_chord(active_notes, options);

            std::string chords_str;
            if (chords.empty()) {
                chords_str = "None";
            } else {
                for (size_t i = 0; i < chords.size(); ++i) {
                    chords_str += chords[i];
                    if (i < chords.size() - 1) chords_str += ", ";
                }
            }

            std::cout << "\r" << notes_str << "Chord: " << chords_str << "                                      ";
            std::cout.flush(); 

            LeaveCriticalSection(&cs); 
        }
    }
}

int main() {
    InitializeCriticalSection(&cs);

    UINT numDevs = midiInGetNumDevs();
    if (numDevs == 0) {
        std::cerr << "No MIDI input devices found!" << std::endl;
        DeleteCriticalSection(&cs);
        return 1;
    }

    std::cout << "Available MIDI Input Devices:\n";
    for (UINT i = 0; i < numDevs; i++) {
        MIDIINCAPSA caps;
        if (midiInGetDevCapsA(i, &caps, sizeof(MIDIINCAPSA)) == MMSYSERR_NOERROR) {
            std::cout << "[" << i << "] " << caps.szPname << "\n";
        }
    }

    std::cout << "\nSelect a device ID to open (0 - " << numDevs - 1 << "): ";
    int deviceId;
    std::cin >> deviceId;

    if (deviceId < 0 || deviceId >= (int)numDevs) {
        std::cerr << "Invalid device ID!" << std::endl;
        DeleteCriticalSection(&cs);
        return 1;
    }

    HMIDIIN hMidiIn;
    MMRESULT result = midiInOpen(&hMidiIn, deviceId, (DWORD_PTR)MidiInProc, 0, CALLBACK_FUNCTION);
    if (result != MMSYSERR_NOERROR) {
        std::cerr << "Failed to open MIDI device." << std::endl;
        DeleteCriticalSection(&cs);
        return 1;
    }

    midiInStart(hMidiIn);

    std::cout << "\nListening to MIDI events... Press [ENTER] to exit.\n\n";
    
    std::cin.ignore(10000, '\n');
    std::cin.get(); 

    midiInStop(hMidiIn);
    midiInClose(hMidiIn);
    DeleteCriticalSection(&cs);

    std::cout << "\nProgram exited." << std::endl;
    return 0;
}