package com.zheliu.pokecapsule.transcription;

import static org.junit.Assert.assertThrows;

import java.io.IOException;

import org.junit.Test;

public final class TranscriptionGuardTest {
    @Test public void rejectsVeryShortAutomaticTranscription() {
        assertThrows(IOException.class,
                () -> TranscriptionGuard.requireTranscribableDuration(1_999));
    }

    @Test public void acceptsEightSecondChinesePhrase() throws IOException {
        TranscriptionGuard.requireTranscribableDuration(8_000);
        TranscriptionGuard.requirePlausibleOutput(
                "福斯特建筑事务所商务提案英文翻译", 8_000);
    }

    @Test public void rejectsOneSecondHallucination() {
        String hallucination =
                "对就好了但是也没有写我拿着来发所以如果算不了这样我要照吧"
                        + "还有带我拍拍全部都是说的现在我只是开着演唱不见到这样有效的啊"
                        + "还很久之后一定要是最多接受目单级指容全部都是逆单级";
        assertThrows(IOException.class,
                () -> TranscriptionGuard.requirePlausibleOutput(hallucination, 1_572));
    }
}
