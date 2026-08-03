package com.zheliu.pokecapsule.service;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.os.Build;

public final class BootReceiver extends BroadcastReceiver {
    @Override public void onReceive(Context context, Intent intent) {
        if (intent == null || !Intent.ACTION_BOOT_COMPLETED.equals(intent.getAction())) return;
        if (!DeviceCapabilities.current().systemOverlayRecorder) return;
        boolean enabled = context.getSharedPreferences("overlay", Context.MODE_PRIVATE)
                .getBoolean("enabled", false);
        if (enabled) {
            Intent service = new Intent(context, OverlayService.class);
            if (Build.VERSION.SDK_INT >= 26) context.startForegroundService(service);
            else context.startService(service);
        }
    }
}
