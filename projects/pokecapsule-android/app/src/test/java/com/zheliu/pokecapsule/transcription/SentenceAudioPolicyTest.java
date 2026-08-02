package com.zheliu.pokecapsule.transcription;

import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

import org.junit.Test;

public final class SentenceAudioPolicyTest {
    @Test public void createsSafeUploadCopyNearTencentLimit() {
        assertFalse(SentenceAudioPolicy.needsUploadCopy(58_999));
        assertTrue(SentenceAudioPolicy.needsUploadCopy(59_000));
        assertTrue(SentenceAudioPolicy.canRepairAutomatically(60_416));
    }

    @Test public void doesNotSilentlyTrimLongImportedAudio() {
        assertFalse(SentenceAudioPolicy.canRepairAutomatically(61_001));
    }

    @Test public void recognizesTencentDurationLimitCode() {
        assertTrue(SentenceAudioPolicy.isDurationLimitError(
                "InvalidParameterValue.ErrorVoicedataTooLong"));
    }
}
