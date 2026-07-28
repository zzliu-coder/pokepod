package com.zheliu.pokecapsule.core;

import static org.junit.Assert.assertEquals;

import org.junit.Test;

public final class AudioLevelTest {
    @Test public void mapsAmplitudeToThreeVisibleLevels() {
        assertEquals(0, AudioLevel.fromAmplitude(0));
        assertEquals(0, AudioLevel.fromAmplitude(299));
        assertEquals(1, AudioLevel.fromAmplitude(300));
        assertEquals(1, AudioLevel.fromAmplitude(1499));
        assertEquals(2, AudioLevel.fromAmplitude(1500));
        assertEquals(2, AudioLevel.fromAmplitude(5999));
        assertEquals(3, AudioLevel.fromAmplitude(6000));
        assertEquals(3, AudioLevel.fromAmplitude(32767));
    }
}
