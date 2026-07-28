package com.zheliu.pokecapsule.core;

public final class AdaptiveVoiceGain {
    private static final double TARGET_RMS = 4_500.0;
    private static final double SPEECH_FLOOR_RMS = 300.0;
    private static final double MAX_GAIN = 4.0;
    private static final int OUTPUT_LIMIT = 30_000;

    private double gain = 1.0;

    public int process(short[] samples, int count) {
        if (count <= 0) return 0;

        double sumSquares = 0.0;
        for (int index = 0; index < count; index++) {
            double sample = samples[index];
            sumSquares += sample * sample;
        }
        double rms = Math.sqrt(sumSquares / count);
        double desired = rms < SPEECH_FLOOR_RMS
                ? 1.0
                : Math.max(1.0, Math.min(MAX_GAIN, TARGET_RMS / rms));

        double response = desired < gain ? 0.85 : 0.08;
        gain += (desired - gain) * response;

        int peak = 0;
        for (int index = 0; index < count; index++) {
            int adjusted = (int) Math.round(samples[index] * gain);
            adjusted = Math.max(-OUTPUT_LIMIT, Math.min(OUTPUT_LIMIT, adjusted));
            samples[index] = (short) adjusted;
            peak = Math.max(peak, Math.abs(adjusted));
        }
        return peak;
    }

    double currentGainForTest() {
        return gain;
    }
}
