package com.zheliu.pokecapsule.service;

import android.content.Context;
import android.content.Intent;

/**
 * Notifies visible app screens after a committed capsule-library change.
 *
 * <p>The broadcast is package-scoped and protected by the app's signature
 * permission. It is event-driven, so it does not add polling or idle work.</p>
 */
public final class LibraryChangeNotifier {
    public static final String ACTION =
            "com.zheliu.pokecapsule.LIBRARY_CHANGED";
    public static final String INTERNAL_PERMISSION =
            "com.zheliu.pokecapsule.permission.INTERNAL";

    private LibraryChangeNotifier() {
    }

    public static void notifyChanged(Context context) {
        Intent intent = new Intent(ACTION);
        intent.setPackage(context.getPackageName());
        context.sendBroadcast(intent, INTERNAL_PERMISSION);
    }
}
