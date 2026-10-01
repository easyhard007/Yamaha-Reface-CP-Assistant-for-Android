package com.chenyinan.reface_cp_assist

import org.junit.Test

import org.junit.Assert.*

/**
 * Example local unit test, which will execute on the development machine (host).
 *
 * See [testing documentation](http://d.android.com/tools/testing).
 */
class ExampleUnitTest {
    @Test
    fun addition_isCorrect() {
        assertEquals(4, 2 + 2)
    }

    @Test
    fun cutPointTimeScalesWithTempo() {
        val at57 = 12.0
        val at114 = DrumLoopPlayer.sourceTimeToTempoTime(57.0, 114.0, at57)
        assertEquals(6.0, at114, 1e-9)
    }

    @Test
    fun cutPointTimeConversionRoundTrips() {
        val sourceTime = 7.345
        val targetTime = DrumLoopPlayer.sourceTimeToTempoTime(57.0, 83.0, sourceTime)
        val restored = DrumLoopPlayer.tempoTimeToSourceTime(57.0, 83.0, targetTime)
        assertEquals(sourceTime, restored, 1e-9)
    }

    @Test
    fun autoFillMapsCurrentMeasurePhaseToLastMeasureCut() {
        val cuts = listOf(
            DrumLoopPlayer.AutoFillCutPoint(3.310, 0.002),
            DrumLoopPlayer.AutoFillCutPoint(3.320, 0.020),
            DrumLoopPlayer.AutoFillCutPoint(9.308, 0.003),
            DrumLoopPlayer.AutoFillCutPoint(9.322, 0.030)
        )

        val transition = DrumLoopPlayer.findAutoFillTransition(
            cuts, positionSec = 3.300, durationSec = 12.0, measureSec = 3.0
        )

        assertNotNull(transition)
        assertEquals(3.309, transition!!.sourceTime, 1e-9)
        assertEquals(9.309, transition.destinationTime, 1e-9)
        assertEquals(0.009, transition.delaySeconds, 1e-9)
    }

    @Test
    fun autoFillDoesNotJumpWhenAlreadyInLastMeasure() {
        val cuts = listOf(DrumLoopPlayer.AutoFillCutPoint(9.100, 0.001))
        assertNull(DrumLoopPlayer.findAutoFillTransition(
            cuts, positionSec = 9.050, durationSec = 12.0, measureSec = 3.0
        ))
    }

    @Test
    fun autoFillWaitsForNextCutPointBeforeMeasureEnd() {
        val cuts = listOf(
            DrumLoopPlayer.AutoFillCutPoint(3.350, 0.001),
            DrumLoopPlayer.AutoFillCutPoint(9.350, 0.001)
        )
        val transition = DrumLoopPlayer.findAutoFillTransition(
            cuts, positionSec = 3.300, durationSec = 12.0, measureSec = 3.0
        )

        assertNotNull(transition)
        assertEquals(3.350, transition!!.sourceTime, 1e-9)
        assertEquals(9.350, transition.destinationTime, 1e-9)
        assertEquals(0.050, transition.delaySeconds, 1e-9)
    }

    @Test
    fun autoFillChoosesEarliestValidPairInsteadOfQuieterLatePair() {
        val cuts = listOf(
            DrumLoopPlayer.AutoFillCutPoint(3.400, 0.100),
            DrumLoopPlayer.AutoFillCutPoint(3.800, 0.001),
            DrumLoopPlayer.AutoFillCutPoint(9.400, 0.100),
            DrumLoopPlayer.AutoFillCutPoint(9.800, 0.001)
        )

        val transition = DrumLoopPlayer.findAutoFillTransition(
            cuts, positionSec = 3.300, durationSec = 12.0, measureSec = 3.0
        )

        assertNotNull(transition)
        assertEquals(3.400, transition!!.sourceTime, 1e-9)
        assertEquals(9.400, transition.destinationTime, 1e-9)
    }

    @Test
    fun autoFillDoesNotJumpWithoutFadeRoomBeforeMeasureEnd() {
        val cuts = listOf(
            DrumLoopPlayer.AutoFillCutPoint(5.980, 0.001),
            DrumLoopPlayer.AutoFillCutPoint(11.980, 0.001)
        )

        assertNull(DrumLoopPlayer.findAutoFillTransition(
            cuts, positionSec = 5.950, durationSec = 12.0, measureSec = 3.0
        ))
    }

    @Test
    fun smartFillRankMapsLoudestMainToLoudestFill() {
        val mains = (1..9).map { "VERSE_%02d".format(it) to it.toDouble() }
        val fills = (1..5).map { "FILLS_%02d".format(it) to it.toDouble() }

        assertEquals(
            "FILLS_05",
            DrumLoopPlayer.findRankMappedVariation("VERSE_09", mains, fills)
        )
    }

    @Test
    fun smartFillRankPreservesBothEndsAndMapsMiddleByQuantile() {
        val mains = (1..9).map { "VERSE_%02d".format(it) to it.toDouble() }
        val fills = (1..5).map { "FILLS_%02d".format(it) to (it * 10.0) }

        assertEquals("FILLS_01", DrumLoopPlayer.findRankMappedVariation("VERSE_01", mains, fills))
        assertEquals("FILLS_02", DrumLoopPlayer.findRankMappedVariation("VERSE_02", mains, fills))
        assertEquals("FILLS_03", DrumLoopPlayer.findRankMappedVariation("VERSE_05", mains, fills))
        assertEquals("FILLS_05", DrumLoopPlayer.findRankMappedVariation("VERSE_09", mains, fills))
    }

    @Test
    fun drumLoopMidiParserKeepsC5TimingAndIgnoresKeyswitchNote() {
        val midi = byteArrayOf(
            0x4D, 0x54, 0x68, 0x64, 0, 0, 0, 6, 0, 0, 0, 1, 1, 0xE0.toByte(),
            0x4D, 0x54, 0x72, 0x6B, 0, 0, 0, 0x1C,
            0, 0xFF.toByte(), 0x51, 3, 0x07, 0xA1.toByte(), 0x20,
            0, 0x90.toByte(), 0x0F, 70,
            0, 0x90.toByte(), 0x3C, 100,
            0x83.toByte(), 0x60, 0x80.toByte(), 0x0F, 0,
            0, 0x80.toByte(), 0x3C, 0,
            0, 0xFF.toByte(), 0x2F, 0
        )

        val phrase = DrumLoopMidiParser.parse(midi)

        assertEquals(2, phrase.events.size)
        assertEquals(0, phrase.c4NoteOnCount)
        assertEquals(1, phrase.c5NoteOnCount)
        assertEquals(0, phrase.connectedC5NoteOnCount)
        assertTrue(phrase.events[0].noteOn)
        assertEquals(100, phrase.events[0].velocity)
        assertEquals(DrumLoopMidiParser.KIND_C5_ISOLATED, phrase.events[0].kind)
        assertEquals(-1, phrase.events[0].phraseGroup)
        assertEquals(0.0, phrase.events[0].timeSeconds, 1e-9)
        assertFalse(phrase.events[1].noteOn)
        assertEquals(DrumLoopMidiParser.KIND_C5_ISOLATED, phrase.events[1].kind)
        assertEquals(0.5, phrase.events[1].timeSeconds, 1e-9)
    }

    @Test
    fun c5GroupingUsesOneBeatInEitherDirectionAndConnectedComponents() {
        val roles = DrumLoopMidiParser.classifyC5Onsets(
            listOf(0L, 480L, 1200L, 1680L, 3000L), beatTicks = 480L
        )

        assertEquals(
            listOf(
                DrumLoopMidiParser.KIND_C5_CONNECTED,
                DrumLoopMidiParser.KIND_C5_CONNECTED,
                DrumLoopMidiParser.KIND_C5_CONNECTED,
                DrumLoopMidiParser.KIND_C5_CONNECTED,
                DrumLoopMidiParser.KIND_C5_ISOLATED
            ),
            roles.map { it.kind }
        )
        assertEquals(listOf(0, 0, 1, 1, -1), roles.map { it.phraseGroup })
    }

    @Test
    fun c5GroupingDoesNotAttachAOneBeatPlusOneTickNeighbor() {
        val roles = DrumLoopMidiParser.classifyC5Onsets(
            listOf(0L, 481L, 960L), beatTicks = 480L
        )

        assertEquals(DrumLoopMidiParser.KIND_C5_ISOLATED, roles[0].kind)
        assertEquals(DrumLoopMidiParser.KIND_C5_CONNECTED, roles[1].kind)
        assertEquals(DrumLoopMidiParser.KIND_C5_CONNECTED, roles[2].kind)
        assertEquals(listOf(-1, 0, 0), roles.map { it.phraseGroup })
    }

    @Test
    fun bassMidiDownbeatOnlyMatchesMeasureStart() {
        val measure = 240.0 / 57.0
        assertTrue(DrumLoopPlayer.isMeasureDownbeat(0.0, measure))
        assertTrue(DrumLoopPlayer.isMeasureDownbeat(measure + 0.010, measure))
        assertFalse(DrumLoopPlayer.isMeasureDownbeat(60.0 / 57.0, measure))
        assertFalse(DrumLoopPlayer.isMeasureDownbeat(measure - 0.050, measure))
        assertFalse(DrumLoopPlayer.isMeasureDownbeat(measure - 0.010, measure))
    }

    @Test
    fun bassRetriggerStrongBeatMatchesBeatOneAndThreeWithinAThirtySecond() {
        val measure = 2.0 // 120 BPM, 4/4: each beat is 0.5 s; a 32nd is 62.5 ms
        assertTrue(DrumLoopPlayer.isBassRetriggerStrongBeat(0.0, measure, 4))
        assertTrue(DrumLoopPlayer.isBassRetriggerStrongBeat(0.050, measure, 4))
        assertTrue(DrumLoopPlayer.isBassRetriggerStrongBeat(1.050, measure, 4))
        assertTrue(DrumLoopPlayer.isBassRetriggerStrongBeat(1.950, measure, 4))
        assertFalse(DrumLoopPlayer.isBassRetriggerStrongBeat(0.070, measure, 4))
        assertFalse(DrumLoopPlayer.isBassRetriggerStrongBeat(0.500, measure, 4))
        assertFalse(DrumLoopPlayer.isBassRetriggerStrongBeat(1.070, measure, 4))
    }
}
