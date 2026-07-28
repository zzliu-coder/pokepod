package com.zheliu.pokecapsule.service;

import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.content.Context;
import android.os.Build;

final class Notifications {
    static final String CHANNEL_OVERLAY = "pokecapsule_overlay";
    static final String CHANNEL_RECORDING = "pokecapsule_recording";

    private Notifications() {}

    static void ensureChannels(Context context) {
        if (Build.VERSION.SDK_INT < 26) return;
        NotificationManager manager =
                (NotificationManager) context.getSystemService(Context.NOTIFICATION_SERVICE);
        if (manager == null) return;
        NotificationChannel overlay = new NotificationChannel(
                CHANNEL_OVERLAY, "PokeCapsule 悬浮按钮", NotificationManager.IMPORTANCE_MIN);
        overlay.setSound(null, null);
        overlay.enableVibration(false);
        manager.createNotificationChannel(overlay);
        NotificationChannel recording = new NotificationChannel(
                CHANNEL_RECORDING, "PokeCapsule 录音", NotificationManager.IMPORTANCE_LOW);
        recording.setSound(null, null);
        recording.enableVibration(false);
        manager.createNotificationChannel(recording);
    }
}
