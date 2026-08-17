package com.zheliu.pokecapsule.service;

import android.Manifest;
import android.content.Context;
import android.content.pm.PackageManager;
import android.os.Build;
import android.os.Environment;

/** One authority for permission checks before touching the shared library. */
public final class LibraryStorageAccess {
    private LibraryStorageAccess() {}

    public static boolean has(Context context) {
        if (context == null) return false;
        int targetSdk = context.getApplicationInfo().targetSdkVersion;
        boolean allFilesGranted = Build.VERSION.SDK_INT >= 30
                && targetSdk >= 30
                && Environment.isExternalStorageManager();
        boolean writeGranted = context.checkSelfPermission(
                Manifest.permission.WRITE_EXTERNAL_STORAGE) == PackageManager.PERMISSION_GRANTED;
        return StorageAccessPolicy.hasAccess(
                Build.VERSION.SDK_INT, targetSdk, allFilesGranted, writeGranted);
    }
}
