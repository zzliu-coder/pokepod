package com.zheliu.pokecapsule.storage;

import static org.junit.Assert.assertEquals;

import com.zheliu.pokecapsule.core.ProcessingState;
import com.zheliu.pokecapsule.model.CapsuleRecord;

import org.junit.Test;

import java.io.File;
import java.util.Arrays;
import java.util.Collections;

public final class SearchIndexTest {
    @Test
    public void searchesFinalTextFolderAndTags() {
        CapsuleRecord record = new CapsuleRecord(
                new File("/tmp/capsule"),
                "0d95b7c1-7ce9-4a91-aea2-b64707a05c9f",
                "语音胶囊",
                "2026-07-28T08:30:00Z",
                "2026-07-28T08:30:00Z",
                3,
                false,
                Arrays.asList("商务", "建筑"),
                ProcessingState.RAW_READY,
                8_000,
                "",
                false,
                "工作/提案",
                "原始文字",
                "",
                "福斯特建筑事务所商务提案英文翻译",
                false,
                "",
                "");

        assertEquals(1, SearchIndex.filter(Collections.singletonList(record), "商务提案").size());
        assertEquals(1, SearchIndex.filter(Collections.singletonList(record), "工作/提案").size());
        assertEquals(1, SearchIndex.filter(Collections.singletonList(record), "建筑").size());
        assertEquals(0, SearchIndex.filter(Collections.singletonList(record), "不存在").size());
    }
}
