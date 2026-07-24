# Yamaha Reface CP Assistant for Android

A native Android assistant app for the **Yamaha Reface CP** keyboard. It connects to the instrument over MIDI and provides real-time chord detection, pitch transposition, smart auto-sustain, bass enhancement, a Cajon rhythm engine with auto-accompaniment, beat tracking with tap tempo, and SoundFont-based audio playback — all driven by a low-latency native audio engine ([Oboe](https://github.com/google/oboe)) and an embedded WebView UI.

This project is an Android port/packaging of the web-based [Yamaha-Reface-CP-Assistant](references/Yamaha-Reface-CP-Assistant), rebuilt with a Kotlin + JNI/C++ native audio stack.

## Features

- 🎹 **Real-time chord detection** — weighted chroma vector template matching with 17 chord types, slash-chord support.
- 🥁 **Cajon rhythm engine** — procedurally generated cajon grooves (bass/tone/tip/slap) with velocity-sensitive round-robin samples and independent per-instrument reverb/EQ.
- 🎸 **Bass assist** — chord-root-driven bass synth with damp/mute articulation, chord-note probability selection, and energy-dependent dynamics.
- 🎵 **Auto-accompaniment** — style-based backing tracks with scene switching (intro/main/fill/ending).
- ⏱️ **Beat tracking & tempo detection** — PLL-based tempo detection with fence-template phase analysis and real-time BPM display on a scrolling scatter plot.
- 👆 **Tap tempo** — tap the sync-zone to set tempo manually; auto-starts rhythm at 50% volume.
- 🔁 **Transpose** — seamless pitch transposition without affecting the played notes.
- ✂️ **Split point** — adjustable keyboard split point.
- 🦶 **Auto-Sustain** — a smart auto-pedal that holds notes musically.
- 🔊 **Bass Enhance** — adds a synthesized bass layer to enrich the low end.
- 🎼 **SoundFont (`.sf2`) playback** — high-quality instrument sound rendered through the native Oboe audio engine (FluidLite).
- 🖥️ **Virtual piano** — on-screen keyboard with real-time note display.

## Requirements

- Android device with **MIDI support** (`android.software.midi`), running **Android 8.1 (API 27)** or higher.
- A USB/MIDI connection to the Yamaha Reface CP (a USB-OTG cable is typically required).
- Android Studio (with NDK & CMake 3.22.1) for building from source.

## ⚠️ Manual SoundFont Setup (Required)

The SoundFont file is **not included** in this repository because of its large size (~356 MB). You must download it manually before building or the app will have no instrument sound.

1. Download the file named **`JJazzLab-SoundFont.sf2`**.
2. Place it in the following folder:

   ```
   Yamaha Reface CP Assistant for Android\app\src\main\assets\
   ```

   The final path should be:

   ```
   app\src\main\assets\JJazzLab-SoundFont.sf2
   ```

3. Rebuild the project. The app loads this SoundFont at runtime to synthesize instrument sounds.

> Most `.sf2` files are listed in `.gitignore` due to their large size, but `033FingerBass-LiveHQNaturalGM.sf2` (bass soundfont) is tracked in the repository.

## ⚠️ Manual Style Files Setup (Optional)

Auto-accompaniment supports **Yamaha `.sty` style files**. Place them in:

```
app\src\main\assets\styles\
```

These style files are also available from **JJazzLab**.

## About JJazzLab

The SoundFont and style files used by this app come from **JJazzLab**, an open-source application. JJazzLab is an open-source project; its source repository is available at:

- **https://github.com/jjazzboss/JJazzLab**

Please refer to the JJazzLab repository and its license for the terms of use of the SoundFont.

## Building

1. Clone this repository.
2. Complete the [Manual SoundFont Setup](#-manual-soundfont-setup-required) step above.
3. Open the project in Android Studio (NDK and CMake 3.22.1 will be configured automatically).
4. Build and run on a MIDI-capable Android device.

> Note: `app/build.gradle.kts` contains a release signing config pointing to a local keystore. Adjust or remove it to match your own signing setup before building a release.

## Usage

1. Launch the app on your device.
2. Open **Settings** (the `?` button), select the **SoundFont** and **Instrument**.
3. Choose the **MIDI port** connected to your Reface CP and press **Start**.
4. Play your Reface CP — chords, rhythm, and bass will respond in real time.

### Accompany Assist Mode (default)

Generates **Cajon percussion** and **chord-driven bass** accompaniment based on your MIDI input:

- **Energy slider** — controls rhythmic density of the Cajon groove.
- **Rhythm volume** — mix level of the Cajon drums. When set to 0, tap the **sync-zone** to set tempo manually (tap tempo); the app analyzes your playing speed to determine BPM automatically.
- **Bass volume** — mix level of the bass synth. When Bass Enhance is disabled on your Reface CP, CC#81 knob controls bass volume instead.
- **Sync-zone** — tap to reset the beat to 1 (rhythm > 0) or tap tempo (rhythm = 0).

### Auto-Accompaniment Mode

Plays **Yamaha `.sty` style files** with structured backing tracks:

1. Place `.sty` files in `app/src/main/assets/styles/` before building.
2. Switch to Auto-Accompaniment mode in the UI.
3. **Select a style** from the list — the app loads its scenes (intro / main / fill / ending).
4. Click a **scene button** to switch sections. The style plays automatically, following the detected chord and tempo.

## Tech Stack

- **Kotlin** + AndroidX
- **Native C++ (JNI)** audio engine with **Oboe** + **FluidLite** for low-latency SoundFont rendering
- **WebView** UI (HTML/CSS/JS, Canvas 2D scatter chart)
- CMake 3.22.1, 16 KB page-size aligned (Android 15+ ready)

## License

See the [LICENSE](LICENSE) file for this project's license. The bundled SoundFont and third-party libraries retain their respective licenses.
