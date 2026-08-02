package com.zheliu.pokecapsule.transcription;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

import org.junit.Test;

public final class TranscriptionErrorTextTest {
    @Test public void durationLimitIsFriendlyAndDoesNotLoop() {
        String code = "InvalidParameterValue.ErrorVoicedataTooLong";
        assertFalse(TranscriptionErrorText.isRetryable(code));
        assertEquals(
                "录音超过转写上限，自动裁剪失败；原录音已保留",
                TranscriptionErrorText.userMessage(code));
    }

    @Test public void transientServiceErrorsRetry() {
        assertTrue(TranscriptionErrorText.isRetryable("InternalError"));
        assertEquals(
                "网络或转写服务暂时不可用，稍后会自动重试",
                TranscriptionErrorText.userMessage("InternalError"));
    }
}
