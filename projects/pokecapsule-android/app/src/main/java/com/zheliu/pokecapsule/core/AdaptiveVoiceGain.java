package com.zheliu.pokecapsule.core;

public final class AdaptiveVoiceGain {
    private static final double READER_TARGET_RMS = 4_500.0;
    private static final double READER_SPEECH_FLOOR_RMS = 300.0;
    private static final double READER_MAX_GAIN = 4.0;
    private static final double PHONE_SPEECH_FLOOR_RMS = 20.0;
    private static final double PHONE_MAX_GAIN = 16.0;
    private static final int OUTPUT_LIMIT = 30_000;
    private static final int SOFT_KNEE = 22_000;

    private final double targetRms;
    private final double speechFloorRms;
    private final double maxGain;
    private final double attack;
    private double gain = 1.0;

    public AdaptiveVoiceGain() {
        this(READER_TARGET_RMS, READER_SPEECH_FLOOR_RMS, READER_MAX_GAIN, 0.08);
    }

    private AdaptiveVoiceGain(
            double targetRms, double speechFloorRms, double maxGain, double attack) {
        this.targetRms = targetRms;
        this.speechFloorRms = speechFloorRms;
        this.maxGain = maxGain;
        this.attack = attack;
    }

    public static AdaptiveVoiceGain forPhone() {
        return new AdaptiveVoiceGain(
                READER_TARGET_RMS, PHONE_SPEECH_FLOOR_RMS, PHONE_MAX_GAIN, 0.25);
    }

    public int process(short[] samples, int count) {
        if (count <= 0) return 0;

        double sumSquares = 0.0;
        for (int index = 0; index < count; index++) {
            double sample = samples[index];
            sumSquares += sample * sample;
        }
        double rms = Math.sqrt(sumSquares / count);
        double desired = rms < speechFloorRms
                ? 1.0
                : Math.max(1.0, Math.min(maxGain, targetRms / rms));

        double response = desired < gain ? 0.85 : attack;
        gain += (desired - gain) * response;

        int peak = 0;
        for (int index = 0; index < count; index++) {
            int adjusted = softLimit(samples[index] * gain);
            samples[index] = (short) adjusted;
            peak = Math.max(peak, Math.abs(adjusted));
        }
        return peak;
    }

    private static int softLimit(double value) {
        double magnitude = Math.abs(value);
        if (magnitude <= SOFT_KNEE) return (int) Math.round(value);
        double span = OUTPUT_LIMIT - SOFT_KNEE;
        double limited = SOFT_KNEE + span * (1.0 - Math.exp(-(magnitude - SOFT_KNEE) / span));
        int result = (int) Math.round(Math.min(OUTPUT_LIMIT, limited));
        return value < 0 ? -result : result;
    }

    double currentGainForTest() {
        return gain;
    }
}
