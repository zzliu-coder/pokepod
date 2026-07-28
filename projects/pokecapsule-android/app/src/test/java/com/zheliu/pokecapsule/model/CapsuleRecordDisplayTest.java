package com.zheliu.pokecapsule.model;

import com.zheliu.pokecapsule.core.ProcessingState;

import org.junit.Test;

import java.io.File;
import java.util.Collections;

import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

public final class CapsuleRecordDisplayTest {
    @Test public void prefersPolishedTextAndHidesCompletedTechnicalState() {
        CapsuleRecord record = new CapsuleRecord(
                new File("/tmp/id"),
                "00000000-0000-0000-0000-000000000000",
                "语音时间",
                "2026-07-28T08:00:00Z",
                "2026-07-28T08:00:00Z",
                true,
                Collections.singletonList("商务"),
                ProcessingState.RAW_READY,
                8_000,
                "",
                false,
                "Inbox",
                "原始转写",
                "校对后的内容");
        String line = record.displayLine();
        assertTrue(line.contains("校对后的内容"));
        assertTrue(line.contains("Inbox"));
        assertTrue(line.contains("#商务"));
        assertFalse(line.contains("raw_ready"));
        assertFalse(line.contains("原始转写"));
    }

    @Test public void pendingCapsuleExplainsAutomaticCondition() {
        CapsuleRecord record = new CapsuleRecord(
                new File("/tmp/id"),
                "00000000-0000-0000-0000-000000000000",
                "",
                "2026-07-28T08:00:00Z",
                "2026-07-28T08:00:00Z",
                false,
                Collections.emptyList(),
                ProcessingState.QUEUED,
                5_000,
                "",
                false,
                "Inbox",
                "",
                "");
        assertTrue(record.displayLine().contains("等待插电和 Wi‑Fi 转写"));
    }
}
