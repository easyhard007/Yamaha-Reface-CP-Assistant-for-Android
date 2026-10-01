package com.chenyinan.reface_cp_assist

import java.util.ArrayDeque

/** Minimal Standard MIDI File reader for drum-loop bass phrases. */
internal object DrumLoopMidiParser {
    // These files use Yamaha/DAW octave names: C4=48 and C5=60.
    internal const val SOURCE_C4 = 48
    internal const val SOURCE_C5 = 60
    internal const val KIND_C4 = 0
    internal const val KIND_C5_ISOLATED = 1
    internal const val KIND_C5_CONNECTED = 2

    internal data class Event(
        val timeSeconds: Double,
        val noteOn: Boolean,
        val velocity: Int,
        val kind: Int,
        /** Connected C5 phrase id, or -1 for C4/isolated C5. */
        val phraseGroup: Int
    )

    internal data class Phrase(
        val events: List<Event>,
        val c4NoteOnCount: Int,
        val c5NoteOnCount: Int,
        val connectedC5NoteOnCount: Int
    )

    private data class RawNote(
        val tick: Long,
        val order: Int,
        val note: Int,
        val noteOn: Boolean,
        val velocity: Int
    )

    private data class Tempo(val tick: Long, val order: Int, val microsPerQuarter: Int)
    internal data class NoteRole(val kind: Int, val phraseGroup: Int)

    internal fun classifyC5Onsets(onsetTicks: List<Long>, beatTicks: Long): List<NoteRole> {
        if (onsetTicks.isEmpty()) return emptyList()
        val result = MutableList(onsetTicks.size) { NoteRole(KIND_C5_ISOLATED, -1) }
        var phraseGroup = 0
        var start = 0
        while (start < onsetTicks.size) {
            var endExclusive = start + 1
            while (endExclusive < onsetTicks.size && beatTicks > 0L &&
                onsetTicks[endExclusive] - onsetTicks[endExclusive - 1] <= beatTicks
            ) {
                endExclusive++
            }
            if (endExclusive - start >= 2) {
                val role = NoteRole(KIND_C5_CONNECTED, phraseGroup++)
                for (index in start until endExclusive) result[index] = role
            }
            start = endExclusive
        }
        return result
    }

    fun parse(data: ByteArray): Phrase {
        val reader = Reader(data)
        require(reader.readAscii(4) == "MThd") { "不是标准 MIDI 文件" }
        val headerLength = reader.readU32().toInt()
        require(headerLength >= 6 && reader.remaining >= headerLength) { "MIDI 头损坏" }
        reader.readU16() // format 0/1/2; all tracks are merged below.
        val trackCount = reader.readU16()
        val division = reader.readU16()
        reader.skip(headerLength - 6)

        val notes = mutableListOf<RawNote>()
        val tempos = mutableListOf<Tempo>()
        var order = 0
        repeat(trackCount) {
            require(reader.readAscii(4) == "MTrk") { "MIDI 轨道块缺失" }
            val length = reader.readU32().toInt()
            require(length >= 0 && reader.remaining >= length) { "MIDI 轨道长度无效" }
            val end = reader.position + length
            var tick = 0L
            var runningStatus = -1
            while (reader.position < end) {
                tick += reader.readVlq()
                var first = reader.readU8()
                val status: Int
                var firstData = -1
                if (first < 0x80) {
                    require(runningStatus in 0x80..0xEF) { "MIDI running status 无效" }
                    status = runningStatus
                    firstData = first
                } else {
                    status = first
                    if (status in 0x80..0xEF) runningStatus = status
                }

                when (status) {
                    0xFF -> {
                        val type = reader.readU8()
                        val metaLength = reader.readVlq().toInt()
                        require(metaLength >= 0 && reader.position + metaLength <= end) {
                            "MIDI meta event 越界"
                        }
                        if (type == 0x51 && metaLength == 3) {
                            val micros = (reader.readU8() shl 16) or
                                (reader.readU8() shl 8) or reader.readU8()
                            if (micros > 0) tempos += Tempo(tick, order++, micros)
                        } else {
                            reader.skip(metaLength)
                        }
                    }
                    0xF0, 0xF7 -> {
                        val sysexLength = reader.readVlq().toInt()
                        require(sysexLength >= 0 && reader.position + sysexLength <= end) {
                            "MIDI SysEx 越界"
                        }
                        reader.skip(sysexLength)
                    }
                    else -> {
                        require(status in 0x80..0xEF) { "不支持的 MIDI 状态 0x${status.toString(16)}" }
                        val command = status and 0xF0
                        val dataLength = if (command == 0xC0 || command == 0xD0) 1 else 2
                        val data1 = if (firstData >= 0) firstData else reader.readU8()
                        val data2 = if (dataLength == 2) reader.readU8() else 0
                        if (command == 0x90 || command == 0x80) {
                            val isOn = command == 0x90 && data2 > 0
                            notes += RawNote(
                                tick = tick,
                                order = order++,
                                note = data1,
                                noteOn = isOn,
                                velocity = if (isOn) data2.coerceIn(1, 127) else 0
                            )
                        }
                    }
                }
            }
            reader.position = end
        }

        val sortedTempos = tempos.sortedWith(compareBy<Tempo>({ it.tick }, { it.order }))
        val selected = notes.asSequence()
            .filter { it.note == SOURCE_C4 || it.note == SOURCE_C5 }
            .sortedWith(compareBy<RawNote>({ it.tick }, { it.order }))
            .toList()
        require(selected.any { it.noteOn }) { "MIDI 中没有 C4/C5 贝斯音符" }

        // C5 classification is variation-local. Adjacent C5 onsets whose distance is
        // at most one quarter-note beat form a connected component. A singleton is an
        // isolated accent; every note in a component of 2+ notes is connected.
        val c5Onsets = selected.filter { it.note == SOURCE_C5 && it.noteOn }
        val roleByOrder = mutableMapOf<Int, NoteRole>()
        val beatTicks = if ((division and 0x8000) == 0) division.toLong() else 0L
        val c5Roles = classifyC5Onsets(c5Onsets.map { it.tick }, beatTicks)
        for (index in c5Onsets.indices) {
            roleByOrder[c5Onsets[index].order] = c5Roles[index]
        }

        // Carry each note-on role to its matching note-off so starting in the middle
        // of a variation can reconstruct the active gate correctly.
        val activeRoles = mutableMapOf<Int, ArrayDeque<NoteRole>>()
        val events = selected.map { note ->
            val role = if (note.noteOn) {
                val value = if (note.note == SOURCE_C4) {
                    NoteRole(KIND_C4, -1)
                } else {
                    roleByOrder[note.order] ?: NoteRole(KIND_C5_ISOLATED, -1)
                }
                activeRoles.getOrPut(note.note) { ArrayDeque() }.addLast(value)
                value
            } else {
                activeRoles[note.note]?.pollFirst()
                    ?: if (note.note == SOURCE_C4) NoteRole(KIND_C4, -1)
                    else NoteRole(KIND_C5_ISOLATED, -1)
            }
            Event(
                timeSeconds = tickToSeconds(note.tick, division, sortedTempos),
                noteOn = note.noteOn,
                velocity = note.velocity,
                kind = role.kind,
                phraseGroup = role.phraseGroup
            )
        }
        return Phrase(
            events = events,
            c4NoteOnCount = selected.count { it.noteOn && it.note == SOURCE_C4 },
            c5NoteOnCount = c5Onsets.size,
            connectedC5NoteOnCount = roleByOrder.values.count {
                it.kind == KIND_C5_CONNECTED
            }
        )
    }

    private fun tickToSeconds(tick: Long, division: Int, tempos: List<Tempo>): Double {
        if ((division and 0x8000) != 0) {
            val signedFps = (division ushr 8).toByte().toInt()
            val fps = when (-signedFps) {
                29 -> 29.97
                else -> (-signedFps).toDouble()
            }
            val ticksPerFrame = division and 0xFF
            require(fps > 0.0 && ticksPerFrame > 0) { "MIDI SMPTE division 无效" }
            return tick.toDouble() / (fps * ticksPerFrame.toDouble())
        }

        require(division > 0) { "MIDI PPQ division 无效" }
        var previousTick = 0L
        var tempo = 500_000
        var micros = 0.0
        for (point in tempos) {
            if (point.tick > tick) break
            if (point.tick > previousTick) {
                micros += (point.tick - previousTick).toDouble() * tempo.toDouble() /
                    division.toDouble()
                previousTick = point.tick
            }
            tempo = point.microsPerQuarter
        }
        if (tick > previousTick) {
            micros += (tick - previousTick).toDouble() * tempo.toDouble() / division.toDouble()
        }
        return micros / 1_000_000.0
    }

    private class Reader(private val data: ByteArray) {
        var position: Int = 0
        val remaining: Int get() = data.size - position

        fun readU8(): Int {
            require(position < data.size) { "MIDI 文件提前结束" }
            return data[position++].toInt() and 0xFF
        }

        fun readU16(): Int = (readU8() shl 8) or readU8()

        fun readU32(): Long = (readU8().toLong() shl 24) or
            (readU8().toLong() shl 16) or (readU8().toLong() shl 8) or readU8().toLong()

        fun readAscii(length: Int): String {
            require(length >= 0 && remaining >= length) { "MIDI 文本块越界" }
            val result = data.copyOfRange(position, position + length).toString(Charsets.US_ASCII)
            position += length
            return result
        }

        fun readVlq(): Long {
            var result = 0L
            repeat(4) {
                val b = readU8()
                result = (result shl 7) or (b and 0x7F).toLong()
                if ((b and 0x80) == 0) return result
            }
            error("MIDI VLQ 超过 4 字节")
        }

        fun skip(length: Int) {
            require(length >= 0 && remaining >= length) { "MIDI 跳转越界" }
            position += length
        }
    }
}
