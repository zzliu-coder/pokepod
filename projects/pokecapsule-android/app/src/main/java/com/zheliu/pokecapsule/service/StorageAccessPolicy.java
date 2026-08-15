package com.zheliu.pokecapsule.service;

/** Pure policy for the two Android storage contracts used by this product. */
public final class StorageAccessPolicy {
    private StorageAccessPolicy() {}

    /**
     * Modern target-SDK variants require the system all-files grant. Legacy
     * Poke3 uses the shared-storage permission granted by its old target.
     */
    public static boolean hasAccess(
            int sdkInt,
            int targetSdk,
            boolean allFilesGranted,
            boolean legacyWritePermissionGranted) {
        if (sdkInt >= 30 && targetSdk >= 30) return allFilesGranted;
        return legacyWritePermissionGranted;
    }
}
