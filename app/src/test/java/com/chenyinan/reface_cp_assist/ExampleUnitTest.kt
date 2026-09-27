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
}
