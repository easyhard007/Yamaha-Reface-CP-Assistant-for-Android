# Yamaha Reface CP Assistant for Android

A native Android companion app for the **Yamaha Reface CP**. It connects to the keyboard through Android MIDI, analyzes notes, chords, key, tempo, and beat position in real time, and combines that information with a low-latency native audio engine.

The current app has two performance modes:

- **Drum Loops (default)** — plays packaged stereo Opus drum-loop variations, time-stretched to the current BPM, with beat-aligned section changes.
- **Cajon Assistant** — the mode previously called “Accompany Assist”; it keeps the original procedural Cajon groove and chord-driven bass assistant.

The former Yamaha `.sty` **Auto-Accompaniment** UI has been replaced by Drum Loops. Its parser and playback engine remain in the source tree as legacy/reference code, but it is not a current user-facing mode.

This project is based on the web version of [Yamaha-Reface-CP-Assistant](references/Yamaha-Reface-CP-Assistant) and is rebuilt around Kotlin, JNI/C++, [Oboe](https://github.com/google/oboe), FluidLite, and a WebView UI.

## Features

- 🥁 **Drum Loops** — packaged song sections such as Verse, Verse 2, Pre-Chorus, Chorus, and Fill, with a pending/playing state and next-measure switching.
- ⚡ **Responsive loop loading** — Android MediaCodec decodes Opus; Signalsmith Stretch provides an immediate low-latency version while SBSMS renders a higher-quality version in the background.
- 🔄 **Tempo matching** — loop audio is time-stretched to the current BPM without intentionally changing pitch. The BPM display has −/+ controls (tap for one step, hold to repeat with system haptics); a −/+ press starts preparation immediately instead of waiting for the UI polling cycle, while the multiplier and hardware knob use the native event path. During playback the prepared tempo is applied only at a safe measure boundary.
- 🎚️ **Smooth playback transitions** — short crossfades between variations, loop-boundary fades, a one-second stop fade, and switch-latency reporting.
- 🪘 **Cajon Assistant** — procedurally generated bass/tone/tip/slap hits using velocity layers and round-robin WAV samples.
- 🎸 **Bass Assist** — chord-driven bass with damp/mute articulation, energy-dependent dynamics, and a dedicated bass SoundFont.
- 🎹 **Real-time chord and key detection** — weighted chroma template matching, slash-chord support, chord tones, and scale/roman-numeral analysis.
- ⏱️ **Beat tracking and tempo detection** — 32nd-note scheduling, tap tempo, BPM display, and a scrolling note/tempo scatter plot.
- 🦶 **Smart Auto-Sustain** — automatically holds notes and briefly releases the pedal when a harmonic collision is detected.
- 🔊 **Bass Enhance** — sends a weighted lower-octave layer back to the Reface CP.
- 🔁 **Transpose and split point** — Reface CP SysEx transposition and an adjustable analysis split point.
- 🎼 **SoundFont lead overlay** — selectable FluidLite instruments mixed through the native Oboe output.
- 🖥️ **Virtual piano** — live note, pedal, chord, and performance-state visualization.

## Requirements

- An Android device with `android.software.midi`, running **Android 8.1 (API 27)** or later.
- A USB/MIDI connection to the Yamaha Reface CP; a USB-OTG adapter is typically required.
- An **arm64-v8a** device for the current build configuration.
- Android Studio with the Android NDK and CMake 3.22.1 when building from source.

## Manual SoundFont Setup (Required)

The main SoundFont is not included because it is approximately 356 MB. Before building:

1. Download `JJazzLab-SoundFont.sf2`.
2. Place it at:

   ```text
   app/src/main/assets/JJazzLab-SoundFont.sf2
   ```

3. Rebuild the app.

Most `.sf2` files are excluded by `.gitignore`. The smaller `033FingerBass-LiveHQNaturalGM.sf2`, used by Bass Assist, is tracked in the repository.

## Drum-Loop Assets

Drum-loop packs live under `app/src/main/assets/drumloops/`. Each pack is a directory containing:

```text
drumloops/
└── PACK_NAME/
    ├── PACK_NAME.json
    ├── VERSE_01.opus
    ├── CHORUS_01.opus
    ├── FILLS_01.opus
    └── ...
```

The JSON metadata supplies the pack BPM, time signature, sample rate, and a `variations` object whose keys match the Opus filenames. The current bundled pack is 48 kHz stereo, 4/4, and contains multiple Intro, Verse, Pre-Chorus, Chorus, Fill, Bridge, Ending, and Pickup variations. The present main grid exposes Verse, Verse 2, Pre-Chorus, Chorus, and Fill; the other section controls are reserved for further development.

Legacy `.sty` files still exist under `app/src/main/assets/styles/` for the retained parser/player code. They are not required for the current Drum Loops mode.

## Building

1. Clone the repository.
2. Complete the [manual SoundFont setup](#manual-soundfont-setup-required).
3. Open the project in Android Studio.
4. Install the requested SDK, NDK, and CMake components if prompted.
5. Build and run on a MIDI-capable arm64 Android device.

`app/build.gradle.kts` currently contains a machine-specific release signing configuration. Replace or remove it before producing your own release build. Do not reuse the repository’s local signing settings for distribution.

## Usage

1. Connect the Reface CP to the Android device and launch the app.
2. Open Settings, select the MIDI device, SoundFont, and overlay instrument, then start the connection.
3. Play the keyboard. Note state, chords, tempo information, and the virtual keyboard update in real time.

### Drum Loops (default)

1. Leave the mode switch on **DRUM LOOPS**.
2. Select a loop pack. The app reads its metadata and loads the Opus assets into memory. The play button stays grey and inactive until a pack and pending variation are fully ready.
3. A default Verse variation is selected. Once **▶** turns green, press it to start playback and reset the beat to measure one.
4. While playing, tap another variation. It becomes pending and switches on the next measure’s first beat.
5. Use **−** or **+** beside the BPM display to change one BPM per tap. Preparation starts as soon as the press reaches Android, without the former 150 ms polling delay. Holding for 0.5 seconds repeats at roughly 50 ms per step and produces keyboard-style haptic feedback when enabled by the device; the first target is prepared immediately, intermediate hold steps are coalesced, and the final target is prepared immediately on release. With no loop playing, the BPM changes immediately. During playback, a held button prevents the prepared tempo from taking effect until release; a request made in the final 1/8 of a measure also skips the imminent boundary, leaving a full extra measure for rendering.
6. Press **■** to stop with a one-second fade.

Variation buttons indicate background preparation state. An unprepared variation can start through the fast Signalsmith path. Once the SBSMS version reaches a safe startup watermark, a pending downbeat switch starts directly from that high-quality buffer; if the fallback is already playing, promotion happens at its loop boundary. A completed SBSMS render becomes the variation's primary cache, so selecting it again cannot restart from a stale low-quality buffer.

### Cajon Assistant

Move the mode switch to **CAJON ASSIST**. This is the retained and renamed former Accompany Assist mode.

- **Volume** controls the Cajon sample mix.
- **Bass** controls the chord-driven bass synth and is mutually exclusive with Bass Enhance.
- **Energy** sets the minimum groove density; played-note density can raise it automatically.
- **Sync zone** resets the beat while the Cajon is audible. With Cajon volume at zero, repeated taps set the tempo; after enough taps, the rhythm starts automatically.

## Architecture

- **Kotlin / Android MIDI** — device connection, WebView bridge, Opus decoding, loop-render scheduling, and application lifecycle.
- **JNI / C++17** — MIDI state, harmony and tempo analysis, beat scheduling, Cajon/Bass generation, time stretching, loop playback, and final audio mixing.
- **Oboe + FluidLite** — low-latency stereo output and SoundFont synthesis.
- **SBSMS + Signalsmith Stretch** — high-quality background and low-latency on-demand time stretching.
- **WebView (HTML/CSS/JavaScript)** — the performance UI and visualizations.

For implementation details, see [技术说明.md](技术说明.md) and [代码文件结构说明.md](代码文件结构说明.md).

## License

See [LICENSE](LICENSE). Bundled samples, SoundFonts, and third-party libraries retain their respective licenses.
