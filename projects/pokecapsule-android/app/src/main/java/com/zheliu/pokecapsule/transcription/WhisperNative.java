package com.zheliu.pokecapsule.transcription;

public final class WhisperNative {
    static {
        System.loadLibrary("pokecapsule_whisper");
    }

    private WhisperNative() {}

    public static native void prepareCurrent();
    public static native String transcribe(String modelPath, float[] pcm16kMono, int threads);
    public static native void cancelCurrent();
}
