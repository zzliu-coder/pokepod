package com.zheliu.pokecapsule.core;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertTrue;

import org.junit.Test;

public final class AdaptiveVoiceGainTest {
    @Test public void silenceStaysSilentAndDoesNotRaiseGain() {
        AdaptiveVoiceGain gain = new AdaptiveVoiceGain();
        short[] samples = new short[320];

        assertEquals(0, gain.process(samples, samples.length));
        assertEquals(1.0, gain.currentGainForTest(), 0.0001);
    }

    @Test public void quietSpeechIsRaisedGradually() {
        AdaptiveVoiceGain gain = new AdaptiveVoiceGain();
        int firstPeak = 0;
        int laterPeak = 0;

        for (int block = 0; block < 20; block++) {
            short[] samples = constantBlock((short) 900);
            int peak = gain.process(samples, samples.length);
            if (block == 0) firstPeak = peak;
            if (block == 19) laterPeak = peak;
        }

        assertTrue(firstPeak > 900);
        assertTrue(laterPeak > firstPeak);
        assertTrue(laterPeak <= 3_600);
    }

    @Test public void suddenLoudSpeechIsLimitedWithoutOverflow() {
        AdaptiveVoiceGain gain = new AdaptiveVoiceGain();
        for (int block = 0; block < 30; block++) {
            short[] quiet = constantBlock((short) 800);
            gain.process(quiet, quiet.length);
        }

        short[] loud = constantBlock((short) 24_000);
        int peak = gain.process(loud, loud.length);

        assertTrue(peak <= 30_000);
        assertEquals(peak, Math.abs(loud[0]));
    }

    private static short[] constantBlock(short value) {
        short[] samples = new short[320];
        for (int index = 0; index < samples.length; index++) samples[index] = value;
        return samples;
    }
}
