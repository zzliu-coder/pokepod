package com.zheliu.pokecapsule.core;

import static org.junit.Assert.assertEquals;

import com.zheliu.pokecapsule.model.CapsuleRecord;

import org.junit.Test;

import java.io.File;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;

public final class LibraryQueryTest {
    @Test public void scopesSearchAndSortAreDerived() {
        CapsuleRecord older = record("a", "2026-01-01T00:00:00Z", "Inbox", false,
                ProcessingState.READY, "第一条", Collections.singletonList("工作"), false);
        CapsuleRecord newer = record("b", "2026-01-02T00:00:00Z", "项目/商务", true,
                ProcessingState.QUEUED, "福斯特提案", Collections.singletonList("建筑"), false);
        CapsuleRecord trashed = record("c", "2026-01-03T00:00:00Z", "Inbox", false,
                ProcessingState.FAILED, "删除内容", Collections.emptyList(), true);
        List<CapsuleRecord> records = Arrays.asList(older, newer, trashed);

        assertEquals(Collections.singletonList("a"), ids(LibraryQuery.apply(records,
                LibraryScope.INBOX, "", LibrarySort.NEWEST_FIRST)));
        assertEquals(Collections.singletonList("b"), ids(LibraryQuery.apply(records,
                LibraryScope.PENDING, "提案", LibrarySort.NEWEST_FIRST)));
        assertEquals(Arrays.asList("b", "a"), ids(LibraryQuery.apply(records,
                LibraryScope.ALL, "", LibrarySort.NEWEST_FIRST)));
        assertEquals(Collections.singletonList("c"), ids(LibraryQuery.apply(records,
                LibraryScope.TRASH, "", LibrarySort.NEWEST_FIRST)));
    }

    private static List<String> ids(List<CapsuleRecord> records) {
        java.util.ArrayList<String> ids = new java.util.ArrayList<>();
        for (CapsuleRecord record : records) ids.add(record.id);
        return ids;
    }

    private static CapsuleRecord record(String id, String date, String folder,
            boolean favorite, ProcessingState state, String raw,
            List<String> tags, boolean trashed) {
        return new CapsuleRecord(new File("/tmp/" + id), id, id, date, date, 1,
                favorite, tags, state, 8000, "", false, folder, raw, "", "",
                trashed, trashed ? date : "", folder);
    }
}
