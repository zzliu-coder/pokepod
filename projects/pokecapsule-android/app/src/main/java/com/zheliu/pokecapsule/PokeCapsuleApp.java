package com.zheliu.pokecapsule;

import android.app.Application;
import android.content.Intent;
import android.os.Build;
import android.provider.Settings;
import android.util.Log;

import com.zheliu.pokecapsule.service.OverlayService;
import com.zheliu.pokecapsule.service.DeviceCapabilities;
import com.zheliu.pokecapsule.service.LibraryStorageAccess;
import com.zheliu.pokecapsule.storage.CapsuleStore;
import com.zheliu.pokecapsule.storage.DeviceIdentity;
import com.zheliu.pokecapsule.storage.PokePaths;
import com.zheliu.pokecapsule.storage.RootWriteLock;

import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

public final class PokeCapsuleApp extends Application {
    private static final CountDownLatch STARTUP_READY = new CountDownLatch(1);
    private final ExecutorService startupExecutor = Executors.newSingleThreadExecutor();

    @Override public void onCreate() {
        super.onCreate();
        boolean lowPowerReader = DeviceCapabilities.current().eink;
        boolean overlayEnabled = getSharedPreferences("overlay", MODE_PRIVATE)
                .getBoolean("enabled", false);
        if (lowPowerReader && overlayEnabled && Settings.canDrawOverlays(this)) {
            Intent overlay = new Intent(this, OverlayService.class);
            if (Build.VERSION.SDK_INT >= 26) startForegroundService(overlay);
            else startService(overlay);
        } else if (!lowPowerReader) {
            getSharedPreferences("overlay", MODE_PRIVATE)
                    .edit()
                    .putBoolean("enabled", false)
                    .apply();
            stopService(new Intent(this, OverlayService.class));
        }
        startupExecutor.execute(() -> {
            try {
                // The modern phone flavor must never scan or mutate the
                // shared library before the user grants all-files access.
                if (!LibraryStorageAccess.has(this)) return;
                PokePaths paths = new PokePaths();
                RootWriteLock.clearLockFromPreviousProcess(paths);
                DeviceIdentity.ensure(this, paths);
                new CapsuleStore(paths).recoverInterruptedWork();
            } catch (Exception error) {
                Log.e("PokeCapsule", "启动初始化失败", error);
                // MainActivity presents permission and storage recovery actions.
            } finally {
                STARTUP_READY.countDown();
            }
        });
    }

    public static boolean awaitStartupReady(long timeoutMs) throws InterruptedException {
        return STARTUP_READY.await(timeoutMs, TimeUnit.MILLISECONDS);
    }
}
