package com.zheliu.pokecapsule.service;

import android.annotation.SuppressLint;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.IntentFilter;
import android.os.Build;

public final class InternalBroadcasts {
    private InternalBroadcasts() {}

    @SuppressLint("UnspecifiedRegisterReceiverFlag")
    public static void register(Context context, BroadcastReceiver receiver,
            IntentFilter filter, String permission) {
        if (Build.VERSION.SDK_INT >= 33) {
            context.registerReceiver(receiver, filter, permission, null,
                    Context.RECEIVER_NOT_EXPORTED);
        } else {
            context.registerReceiver(receiver, filter, permission, null);
        }
    }
}
