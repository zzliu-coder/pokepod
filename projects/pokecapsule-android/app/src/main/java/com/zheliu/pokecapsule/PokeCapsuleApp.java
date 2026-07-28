package com.zheliu.pokecapsule;

import android.app.Application;
import android.content.Intent;
import android.os.Build;
import android.provider.Settings;

import com.zheliu.pokecapsule.service.OverlayService;
import com.zheliu.pokecapsule.service.TranscriptionScheduler;
import com.zheliu.pokecapsule.storage.CapsuleStore;
import com.zheliu.pokecapsule.storage.PokePaths;

import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

public final class PokeCapsuleApp extends Application {
    private final ExecutorService startupExecutor = Executors.newSingleThreadExecutor();

    @Override public void onCreate() {
        super.onCreate();
        boolean overlayEnabled = getSharedPreferences("overlay", MODE_PRIVATE)
                .getBoolean("enabled", false);
        if (overlayEnabled && Settings.canDrawOverlays(this)) {
            Intent overlay = new Intent(this, OverlayService.class);
            if (Build.VERSION.SDK_INT >= 26) startForegroundService(overlay);
            else startService(overlay);
        }
        startupExecutor.execute(() -> {
            try {
                new CapsuleStore(new PokePaths()).recoverInterruptedWork();
                TranscriptionScheduler.scheduleAutomatic(this);
            } catch (Exception ignored) {
                // MainActivity presents permission and storage recovery actions.
            }
        });
    }
}
