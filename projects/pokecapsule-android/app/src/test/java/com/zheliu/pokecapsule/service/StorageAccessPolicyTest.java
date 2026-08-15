package com.zheliu.pokecapsule.service;

import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

import org.junit.Test;

public final class StorageAccessPolicyTest {
    @Test public void modernRequiresAllFilesGrant() {
        assertFalse(StorageAccessPolicy.hasAccess(34, 34, false, true));
        assertTrue(StorageAccessPolicy.hasAccess(34, 34, true, false));
    }

    @Test public void legacyUsesWritePermission() {
        assertTrue(StorageAccessPolicy.hasAccess(29, 28, false, true));
        assertFalse(StorageAccessPolicy.hasAccess(29, 28, true, false));
    }

    @Test public void targetThirtyOnOldRuntimeKeepsLegacyPath() {
        assertTrue(StorageAccessPolicy.hasAccess(29, 34, false, true));
    }
}
