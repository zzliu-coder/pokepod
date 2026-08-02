package com.zheliu.pokecapsule.transcription;

public final class SentenceAudioPolicy {
    public static final long SAFE_CAPTURE_DURATION_MS = 59_000;
    public static final long MAX_AUTOMATIC_REPAIR_DURATION_MS = 61_000;

    private SentenceAudioPolicy() {}

    public static boolean needsUploadCopy(long durationMs) {
        return durationMs >= SAFE_CAPTURE_DURATION_MS;
    }

    public static boolean canRepairAutomatically(long durationMs) {
        return durationMs > 0 && durationMs <= MAX_AUTOMATIC_REPAIR_DURATION_MS;
    }

    public static boolean isDurationLimitError(String code) {
        return code != null && code.contains("ErrorVoicedataTooLong");
    }
}
