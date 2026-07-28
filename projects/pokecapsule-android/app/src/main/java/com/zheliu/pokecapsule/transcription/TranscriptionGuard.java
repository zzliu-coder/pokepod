package com.zheliu.pokecapsule.transcription;

import java.io.IOException;

public final class TranscriptionGuard {
    public static final long MIN_AUTOMATIC_DURATION_MS = 2_000;
    private static final int MAX_CHARACTERS_PER_SECOND = 10;
    private static final int MINIMUM_CHARACTER_LIMIT = 24;

    private TranscriptionGuard() {}

    public static void requireTranscribableDuration(long durationMs) throws IOException {
        if (durationMs < MIN_AUTOMATIC_DURATION_MS) {
            throw new IOException("录音不足 2 秒，已保留音频，未自动转写");
        }
    }

    public static void requirePlausibleOutput(String text, long durationMs) throws IOException {
        String compact = text == null ? "" : text.replaceAll("\\s+", "");
        int characters = compact.codePointCount(0, compact.length());
        int limit = Math.max(
                MINIMUM_CHARACTER_LIMIT,
                (int) Math.ceil(durationMs / 1000.0 * MAX_CHARACTERS_PER_SECOND));
        if (characters > limit) {
            throw new IOException(
                    "转写结果异常："
                            + Math.max(1, Math.round(durationMs / 1000f))
                            + " 秒音频生成 "
                            + characters
                            + " 字，已拦截幻觉文本");
        }
    }
}
