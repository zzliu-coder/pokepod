package com.zheliu.pokecapsule.core;

public final class AudioLevel {
    private AudioLevel() {}

    public static int fromAmplitude(int amplitude) {
        if (amplitude < 300) return 0;
        if (amplitude < 1_500) return 1;
        if (amplitude < 6_000) return 2;
        return 3;
    }
}
