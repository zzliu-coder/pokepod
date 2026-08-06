package com.zheliu.pokecapsule.core;

import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

import org.junit.Test;

public final class AudioFilePolicyTest {
    @Test public void acceptsLegacyM4aAndBothV2Formats() {
        assertTrue(AudioFilePolicy.isSupportedMetadata(
                1, "audio.m4a", "", 0, 0, 0));
        assertTrue(AudioFilePolicy.isSupportedMetadata(
                2, "audio.m4a", "m4a-aac-lc", 16_000, 1, 16));
        assertTrue(AudioFilePolicy.isSupportedMetadata(
                2, "audio.wav", "wav-pcm-s16le", 16_000, 1, 16));
    }

    @Test public void rejectsTraversalUnknownSchemaAndMismatchedFormat() {
        assertFalse(AudioFilePolicy.isSafeBasename("../audio.wav"));
        assertFalse(AudioFilePolicy.isSafeBasename("nested/audio.m4a"));
        assertFalse(AudioFilePolicy.isSupportedMetadata(
                3, "audio.wav", "wav-pcm-s16le", 16_000, 1, 16));
        assertFalse(AudioFilePolicy.isSupportedMetadata(
                2, "audio.wav", "m4a-aac-lc", 16_000, 1, 16));
        assertFalse(AudioFilePolicy.isSupportedMetadata(
                2, "audio.wav", "wav-pcm-s16le", 16_000, 1, 24));
    }
}
