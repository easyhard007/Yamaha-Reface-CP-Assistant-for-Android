#!/usr/bin/env python3
"""Generate one continuous bass MIDI from one continuous drum MIDI.

MIDI-GPT's yellow_small checkpoint accepts 8-bar scores. Long input is fed to
the model through overlapping 8-bar windows, but the result is joined into one
continuous MIDI file. The overlap is model context only: no variation is split
into separate output files.

The model is conditioned by the source drum performance and a sustained C-major
chord track. Generated pitch, onset, duration and velocity are preserved; in
particular, pitches are never collapsed to C4.
"""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import math
from dataclasses import dataclass
from pathlib import Path

import mido
from midigpt import Bar, Note, Score, Track
from midigpt.inference import (
    GenerationRequest,
    InferenceConfig,
    InferenceEngine,
    TrackPrompt,
)


MODEL_RESOLUTION = 1920
MODEL_BARS = 8
GM_FINGER_BASS = 33  # zero-based program number: Electric Bass (finger)
C_MAJOR_CHORD = (48, 52, 55)


@dataclass(frozen=True)
class DrumHit:
    tick: int
    note: int
    velocity: int


@dataclass(frozen=True)
class BassNote:
    start: int
    end: int
    pitch: int
    velocity: int


@dataclass
class SourceMidi:
    path: Path
    midi: mido.MidiFile
    ticks_per_beat: int
    tempo: int
    numerator: int
    denominator: int
    measure_ticks: int
    total_ticks: int
    bars: int
    drum_track_index: int
    hits: list[DrumHit]


def read_source(path: Path) -> SourceMidi:
    midi = mido.MidiFile(path)
    if midi.ticks_per_beat <= 0:
        raise ValueError("SMPTE MIDI is not supported")

    tempo = 500_000
    numerator = 4
    denominator = 4
    max_tick = 0
    track_hits: list[list[DrumHit]] = []

    for track in midi.tracks:
        absolute = 0
        hits: list[DrumHit] = []
        for message in track:
            absolute += message.time
            max_tick = max(max_tick, absolute)
            if message.type == "set_tempo":
                if absolute != 0 and message.tempo != tempo:
                    raise ValueError("tempo changes are not supported by this checkpoint")
                tempo = message.tempo
            elif message.type == "time_signature":
                if absolute != 0 and (
                    message.numerator != numerator
                    or message.denominator != denominator
                ):
                    raise ValueError("time-signature changes are not supported")
                numerator = message.numerator
                denominator = message.denominator
            elif message.type == "note_on" and message.velocity > 0:
                hits.append(
                    DrumHit(
                        tick=absolute,
                        note=max(0, min(127, message.note)),
                        velocity=max(1, min(127, message.velocity)),
                    )
                )
        track_hits.append(hits)

    if not track_hits or not any(track_hits):
        raise ValueError(f"no drum note-on events found in {path}")
    drum_track_index = max(range(len(track_hits)), key=lambda i: len(track_hits[i]))
    hits = track_hits[drum_track_index]
    measure_ticks = round(midi.ticks_per_beat * numerator * 4 / denominator)
    if measure_ticks <= 0:
        raise ValueError("invalid time signature")
    bars = max(1, int(math.ceil(max_tick / measure_ticks - 1e-9)))
    total_ticks = bars * measure_ticks

    return SourceMidi(
        path=path,
        midi=midi,
        ticks_per_beat=midi.ticks_per_beat,
        tempo=tempo,
        numerator=numerator,
        denominator=denominator,
        measure_ticks=measure_ticks,
        total_ticks=total_ticks,
        bars=bars,
        drum_track_index=drum_track_index,
        hits=hits,
    )


def model_measure_ticks(source: SourceMidi) -> int:
    return round(MODEL_RESOLUTION * source.numerator * 4 / source.denominator)


def stable_window_seed(base_seed: int, source: Path, start_bar: int) -> int:
    value = f"{base_seed}:{source.name}:{start_bar}".encode("utf-8")
    digest = hashlib.sha256(value).digest()
    return int.from_bytes(digest[:4], "big") & 0x7FFF_FFFF


def add_context_notes(
    bass_bars: list[Bar],
    notes: list[BassNote],
    window_start_tick: int,
    context_end_tick: int,
    measure_ticks: int,
) -> None:
    for note in notes:
        if note.end <= window_start_tick or note.start >= context_end_tick:
            continue
        clipped_start = max(note.start, window_start_tick)
        clipped_end = min(note.end, context_end_tick)
        if clipped_end <= clipped_start:
            continue
        relative_start = clipped_start - window_start_tick
        bar_index = min(len(bass_bars) - 1, relative_start // measure_ticks)
        onset = relative_start - bar_index * measure_ticks
        bass_bars[bar_index].notes.append(
            Note(
                pitch=note.pitch,
                velocity=note.velocity,
                onset_ticks=onset,
                duration_ticks=clipped_end - clipped_start,
            )
        )


def build_window_score(
    source: SourceMidi,
    generated_notes: list[BassNote],
    start_bar: int,
    context_bars: int,
) -> Score:
    measure = model_measure_ticks(source)
    source_window_start = start_bar * source.measure_ticks
    source_window_end = (start_bar + MODEL_BARS) * source.measure_ticks
    drum_bars = [
        Bar(ts_numerator=source.numerator, ts_denominator=source.denominator)
        for _ in range(MODEL_BARS)
    ]

    for hit in source.hits:
        if hit.tick < source_window_start:
            continue
        if hit.tick >= source_window_end:
            break
        relative_source_tick = hit.tick - source_window_start
        relative_model_tick = round(
            relative_source_tick * MODEL_RESOLUTION / source.ticks_per_beat
        )
        bar_index = min(MODEL_BARS - 1, relative_model_tick // measure)
        onset = relative_model_tick - bar_index * measure
        drum_bars[bar_index].notes.append(
            Note(
                pitch=hit.note,
                velocity=hit.velocity,
                onset_ticks=onset,
                duration_ticks=max(1, MODEL_RESOLUTION // 16),
            )
        )

    chord_bars = [
        Bar(
            notes=[
                Note(
                    pitch=pitch,
                    velocity=58,
                    onset_ticks=0,
                    duration_ticks=measure - 1,
                )
                for pitch in C_MAJOR_CHORD
            ],
            ts_numerator=source.numerator,
            ts_denominator=source.denominator,
        )
        for _ in range(MODEL_BARS)
    ]
    bass_bars = [
        Bar(ts_numerator=source.numerator, ts_denominator=source.denominator)
        for _ in range(MODEL_BARS)
    ]
    window_model_start = start_bar * measure
    context_model_end = window_model_start + context_bars * measure
    add_context_notes(
        bass_bars,
        generated_notes,
        window_model_start,
        context_model_end,
        measure,
    )

    return Score(
        resolution=MODEL_RESOLUTION,
        tempo=source.tempo,
        tracks=[
            Track(bars=drum_bars, instrument=0, track_type="drum"),
            Track(bars=chord_bars, instrument=0, track_type="melodic"),
            Track(bars=bass_bars, instrument=GM_FINGER_BASS, track_type="melodic"),
        ],
    )


def generate_window(
    engine: InferenceEngine,
    score: Score,
    context_bars: int,
    seed: int,
) -> Track:
    request = GenerationRequest(
        tracks=[
            TrackPrompt(id=0, bars=[]),
            TrackPrompt(id=1, bars=[]),
            TrackPrompt(
                id=2,
                bars=list(range(context_bars, MODEL_BARS)),
                autoregressive=True,
                attributes={
                    "min_polyphony": 0,
                    "max_polyphony": 0,
                    "min_note_duration": 1,
                    "max_note_duration": 5,
                },
            ),
        ],
        config=InferenceConfig(
            temperature=0.92,
            seed=seed,
            max_attempts=4,
            novelty_check=False,
            silence_check=True,
            bars_per_step=1,
            tracks_per_step=1,
            model_dim=MODEL_BARS,
            mask_mode="attention",
            polyphony_hard_limit=1,
            top_p=0.95,
            top_k=64,
        ),
    )
    generated = engine.session(copy.deepcopy(score), request).run()
    return generated.tracks[2]


def collect_new_notes(
    source: SourceMidi,
    track: Track,
    start_bar: int,
    context_bars: int,
    new_bars: int,
) -> list[BassNote]:
    measure = model_measure_ticks(source)
    first_bar = context_bars
    last_bar = min(MODEL_BARS, context_bars + new_bars)
    segment_start = (start_bar + first_bar) * measure
    segment_end = (start_bar + last_bar) * measure
    result: list[BassNote] = []

    for bar_index in range(first_bar, last_bar):
        for note in track.bars[bar_index].notes:
            start = (start_bar + bar_index) * measure + note.onset_ticks
            end = start + max(1, note.duration_ticks)
            start = max(segment_start, min(segment_end, start))
            end = max(start + 1, min(segment_end, end))
            if start < segment_end and end > start:
                result.append(
                    BassNote(
                        start=start,
                        end=end,
                        pitch=max(0, min(127, note.pitch)),
                        velocity=max(1, min(127, note.velocity)),
                    )
                )
    return result


def make_monophonic(notes: list[BassNote], total_ticks: int) -> list[BassNote]:
    ordered = sorted(notes, key=lambda n: (n.start, n.pitch, n.end))
    result: list[BassNote] = []
    for note in ordered:
        if note.start >= total_ticks:
            continue
        current = BassNote(
            start=max(0, note.start),
            end=min(total_ticks, max(note.start + 1, note.end)),
            pitch=note.pitch,
            velocity=note.velocity,
        )
        if result and current.start < result[-1].end:
            previous = result[-1]
            if current.start == previous.start:
                # The model is constrained to one voice, but if it emits a same-onset
                # duplicate keep the stronger note without inventing a new pitch.
                if current.velocity > previous.velocity:
                    result[-1] = current
                continue
            result[-1] = BassNote(
                previous.start,
                current.start,
                previous.pitch,
                previous.velocity,
            )
        result.append(current)
    return [note for note in result if note.end > note.start]


def preserve_meta_track(source: SourceMidi) -> mido.MidiTrack:
    events: list[tuple[int, int, mido.MetaMessage]] = []
    sequence = 0
    for track in source.midi.tracks:
        absolute = 0
        for message in track:
            absolute += message.time
            if not message.is_meta or message.type in ("end_of_track", "track_name"):
                continue
            events.append((absolute, sequence, copy.copy(message)))
            sequence += 1
    events.sort(key=lambda item: (item[0], item[1]))

    track = mido.MidiTrack()
    track.append(mido.MetaMessage("track_name", name="MIDI-GPT Bass markers", time=0))
    previous = 0
    for tick, _, message in events:
        message.time = tick - previous
        track.append(message)
        previous = tick
    track.append(
        mido.MetaMessage("end_of_track", time=max(0, source.total_ticks - previous))
    )
    return track


def write_output(path: Path, source: SourceMidi, notes: list[BassNote]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    midi = mido.MidiFile(type=1, ticks_per_beat=source.ticks_per_beat)
    midi.tracks.append(preserve_meta_track(source))

    scale = source.ticks_per_beat / MODEL_RESOLUTION
    events: list[tuple[int, int, mido.Message]] = []
    for note in notes:
        start = max(0, min(source.total_ticks, round(note.start * scale)))
        if start >= source.total_ticks:
            continue
        end = min(
            source.total_ticks,
            max(start + 1, round(note.end * scale)),
        )
        events.append(
            (
                start,
                1,
                mido.Message(
                    "note_on", channel=0, note=note.pitch, velocity=note.velocity
                ),
            )
        )
        events.append(
            (
                end,
                0,
                mido.Message("note_off", channel=0, note=note.pitch, velocity=0),
            )
        )
    events.sort(key=lambda item: (item[0], item[1]))

    bass = mido.MidiTrack()
    bass.append(mido.MetaMessage("track_name", name="MIDI-GPT Finger Bass", time=0))
    bass.append(mido.Message("program_change", channel=0, program=GM_FINGER_BASS, time=0))
    previous = 0
    for tick, _, message in events:
        message.time = tick - previous
        bass.append(message)
        previous = tick
    bass.append(
        mido.MetaMessage("end_of_track", time=max(0, source.total_ticks - previous))
    )
    midi.tracks.append(bass)
    midi.save(path)


def generate(
    engine: InferenceEngine,
    source: SourceMidi,
    overlap_bars: int,
    base_seed: int,
) -> tuple[list[BassNote], list[dict[str, int]]]:
    notes: list[BassNote] = []
    windows: list[dict[str, int]] = []
    produced_bars = 0
    window_number = 0

    while produced_bars < source.bars:
        context_bars = 0 if produced_bars == 0 else min(overlap_bars, produced_bars)
        start_bar = produced_bars - context_bars
        capacity = MODEL_BARS - context_bars
        new_bars = min(capacity, source.bars - produced_bars)
        seed = stable_window_seed(base_seed, source.path, start_bar)
        score = build_window_score(source, notes, start_bar, context_bars)
        bass_track = generate_window(engine, score, context_bars, seed)
        new_notes = collect_new_notes(
            source, bass_track, start_bar, context_bars, new_bars
        )
        notes.extend(new_notes)
        produced_bars += new_bars
        window_number += 1
        windows.append(
            {
                "window": window_number,
                "start_bar": start_bar,
                "context_bars": context_bars,
                "generated_bars": new_bars,
                "seed": seed,
                "notes": len(new_notes),
            }
        )
        print(
            f"window {window_number}: bars {start_bar + 1}-"
            f"{start_bar + context_bars + new_bars} "
            f"(context={context_bars}, new={new_bars}, notes={len(new_notes)})",
            flush=True,
        )

    total_model_ticks = source.bars * model_measure_ticks(source)
    return make_monophonic(notes, total_model_ticks), windows


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Generate one continuous MIDI-GPT bass track from drum MIDI"
    )
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--checkpoint", type=Path, required=True)
    parser.add_argument("--device", default="cuda")
    parser.add_argument("--seed", type=int, default=20260929)
    parser.add_argument("--overlap-bars", type=int, default=2)
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    if not 0 <= args.overlap_bars < MODEL_BARS:
        raise SystemExit(f"--overlap-bars must be between 0 and {MODEL_BARS - 1}")
    source = read_source(args.input)
    print(
        f"source: {source.path} | bars={source.bars} | hits={len(source.hits)} | "
        f"drum_track={source.drum_track_index}",
        flush=True,
    )
    engine = InferenceEngine.from_checkpoint(str(args.checkpoint), device=args.device)
    notes, windows = generate(engine, source, args.overlap_bars, args.seed)
    write_output(args.output, source, notes)

    unique_pitches = sorted({note.pitch for note in notes})
    report = {
        "source": str(source.path),
        "output": str(args.output),
        "bars": source.bars,
        "tempo_bpm": 60_000_000 / source.tempo,
        "time_signature": f"{source.numerator}/{source.denominator}",
        "drum_track_index": source.drum_track_index,
        "drum_hits": len(source.hits),
        "bass_notes": len(notes),
        "unique_pitches": unique_pitches,
        "pitch_min": min(unique_pitches) if unique_pitches else None,
        "pitch_max": max(unique_pitches) if unique_pitches else None,
        "model": "Metacreation/MIDI-GPT yellow_small",
        "harmony_condition": "C major",
        "pitch_postprocessing": "none",
        "overlap_bars": args.overlap_bars,
        "windows": windows,
    }
    report_path = args.output.with_suffix(".json")
    report_path.write_text(
        json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8"
    )
    print(f"output: {args.output}", flush=True)
    print(f"report: {report_path}", flush=True)
    print(
        f"bass notes={len(notes)}, unique pitches={len(unique_pitches)}, "
        f"range={report['pitch_min']}..{report['pitch_max']}",
        flush=True,
    )


if __name__ == "__main__":
    main()
