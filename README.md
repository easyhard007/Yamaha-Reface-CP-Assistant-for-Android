# Yamaha Reface CP Assistant for Android

A native Android assistant app for the **Yamaha Reface CP** keyboard. It connects to the instrument over MIDI and provides real-time chord/scale visualization, pitch transposition, a smart auto-sustain pedal, bass enhancement, and SoundFont-based audio playback — all driven by a low-latency native audio engine ([Oboe](https://github.com/google/oboe)) and an embedded web UI.

This project is an Android port/packaging of the web-based [Yamaha-Reface-CP-Assistant](references/Yamaha-Reface-CP-Assistant), rebuilt with a Kotlin + JNI/C++ native audio stack.

## Features

- 🎹 **Real-time chord & key tracking** — TSD (Tonic / Subdominant / Dominant) chord-function visualization with live key/scale detection.
- 🌈 **Color-mapped light panel** — chords and keys mapped to colors on an interactive light panel.
- 🔁 **Transpose** — seamless pitch transposition without affecting the played notes.
- ✂️ **Split point** — adjustable keyboard split point.
- 🦶 **Auto-Sustain** — a smart auto-pedal that holds notes musically.
- 🔊 **Bass Enhance** — adds a synthesized bass layer to enrich the low end.
- 🎼 **SoundFont (`.sf2`) playback** — high-quality instrument sound rendered through the native Oboe audio engine.
- 🖥️ **Virtual piano & MIDI monitor** — on-screen keyboard plus a live MIDI debug monitor.

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

> The `.sf2` file is listed in `.gitignore`, so it will not be tracked by Git even after you add it locally.

## About JJazzLab

The SoundFont used by this app comes from **JJazzLab**, an open-source application. JJazzLab is an open-source project; its source repository is available at:

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
2. Open **Settings** (the `?` button).
3. Select the **SoundFont** and **Instrument**.
4. Choose the **MIDI port** connected to your Reface CP and press **Start**.
5. Play your Reface CP — chords, keys, the light panel, and audio output will respond in real time.

## Tech Stack

- **Kotlin** + AndroidX
- **Native C++ (JNI)** audio engine with **Oboe** for low-latency SoundFont rendering
- **WebView** UI (HTML/CSS/JS) with [Tonal.js](https://github.com/tonaljs/tonal), [Three.js](https://threejs.org/), and [iro.js](https://iro.js.org/)
- CMake 3.22.1, 16 KB page-size aligned (Android 15+ ready)

## License

See the [LICENSE](LICENSE) file for this project's license. The bundled SoundFont and third-party libraries retain their respective licenses.
