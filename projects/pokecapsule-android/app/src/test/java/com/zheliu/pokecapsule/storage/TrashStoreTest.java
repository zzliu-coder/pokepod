package com.zheliu.pokecapsule.storage;

import org.junit.Test;

import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

public final class TrashStoreTest {
    @Test public void bindsTrashDirectoryNameToCapsuleUuid() {
        String id = "00000000-0000-0000-0000-000000000000";
        assertTrue(TrashStore.directoryNameMatchesId(id, id));
        assertTrue(TrashStore.directoryNameMatchesId(id + "-legacy", id));
        assertFalse(TrashStore.directoryNameMatchesId("11111111-1111-1111-1111-111111111111", id));
        assertFalse(TrashStore.directoryNameMatchesId("prefix-" + id, id));
    }
}
