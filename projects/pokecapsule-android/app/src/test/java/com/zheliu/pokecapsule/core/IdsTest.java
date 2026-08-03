package com.zheliu.pokecapsule.core;

import org.junit.Test;

import java.util.HashSet;
import java.util.Set;

import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNotEquals;
import static org.junit.Assert.assertTrue;

public final class IdsTest {
    @Test public void validatesUuid() {
        assertTrue(Ids.isUuid("0d95b7c1-7ce9-4a91-aea2-b64707a05c9f"));
        assertFalse(Ids.isUuid("../Inbox"));
        assertFalse(Ids.isUuid("not-a-uuid"));
    }

    @Test public void oneHundredCopiesAreUnique() {
        String source = "0d95b7c1-7ce9-4a91-aea2-b64707a05c9f";
        Set<String> values = new HashSet<>();
        for (int index = 0; index < 100; index++) {
            String copy = Ids.newCopyId(source);
            assertTrue(Ids.isUuid(copy));
            assertNotEquals(source, copy);
            assertTrue(values.add(copy));
        }
    }

    @Test public void normalizesUppercaseProtocolUuidForFilesystemUse() {
        assertEquals(
                "4010f256-ce50-4db9-98d9-e1076d89f061",
                Ids.normalized("4010F256-CE50-4DB9-98D9-E1076D89F061"));
    }
}
