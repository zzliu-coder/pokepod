package com.zheliu.pokecapsule.core;

import java.io.File;
import java.io.IOException;

public final class AudioFilePolicy {
    public static final String ANDROID_AUDIO_FILE = "audio.m4a";
    public static final String POKEPOD_AUDIO_FILE = "audio.wav";
    public static final String M4A_AAC_LC = "m4a-aac-lc";
    public static final String WAV_PCM_S16LE = "wav-pcm-s16le";

    private AudioFilePolicy() {}

    public static boolean isSafeBasename(String value) {
        return ANDROID_AUDIO_FILE.equals(value) || POKEPOD_AUDIO_FILE.equals(value);
    }

    public static boolean isSupportedMetadata(
            int schemaVersion,
            String audioFile,
            String audioFormat,
            int sampleRateHz,
            int channels,
            int bitsPerSample) {
        if (!isSafeBasename(audioFile)) return false;
        if (schemaVersion == 1) return ANDROID_AUDIO_FILE.equals(audioFile);
        if (schemaVersion != 2 || sampleRateHz < 8_000 || sampleRateHz > 192_000
                || channels < 1 || channels > 8 || bitsPerSample < 8 || bitsPerSample > 32) {
            return false;
        }
        if (ANDROID_AUDIO_FILE.equals(audioFile)) return M4A_AAC_LC.equals(audioFormat);
        return WAV_PCM_S16LE.equals(audioFormat) && bitsPerSample == 16;
    }

    public static File resolve(File capsuleDirectory, String audioFile) throws IOException {
        if (!isSafeBasename(audioFile)) throw new IOException("音频文件名不安全");
        File resolved = new File(capsuleDirectory, audioFile).getCanonicalFile();
        if (!capsuleDirectory.getCanonicalFile().equals(resolved.getParentFile())) {
            throw new IOException("音频文件超出胶囊目录");
        }
        return resolved;
    }
}
