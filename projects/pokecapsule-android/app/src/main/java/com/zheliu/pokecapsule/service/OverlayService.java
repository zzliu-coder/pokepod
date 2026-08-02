package com.zheliu.pokecapsule.service;

import android.annotation.SuppressLint;
import android.app.Notification;
import android.app.PendingIntent;
import android.app.Service;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.res.Configuration;
import android.graphics.PixelFormat;
import android.os.Build;
import android.os.IBinder;
import android.provider.Settings;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.view.WindowManager;

import com.zheliu.pokecapsule.ui.CapsuleRecordButtonView;
import com.zheliu.pokecapsule.ui.MainActivity;

public final class OverlayService extends Service {
    public static final String ACTION_HIDE = "com.zheliu.pokecapsule.OVERLAY_HIDE";
    public static final String ACTION_DISABLE = "com.zheliu.pokecapsule.OVERLAY_DISABLE";
    private static final int NOTIFICATION_ID = 4201;

    private WindowManager windowManager;
    private WindowManager.LayoutParams layout;
    private CapsuleRecordButtonView button;
    private boolean recording;
    private float downX;
    private float downY;
    private int startX;
    private int startY;
    private boolean moved;

    private final BroadcastReceiver stateReceiver = new BroadcastReceiver() {
        @Override public void onReceive(Context context, Intent intent) {
            recording = intent.getBooleanExtra(RecordingService.EXTRA_RECORDING, false);
            int seconds = intent.getIntExtra(RecordingService.EXTRA_SECONDS_LEFT, 0);
            int audioLevel = intent.getIntExtra(RecordingService.EXTRA_AUDIO_LEVEL, 0);
            boolean silent = intent.getBooleanExtra(RecordingService.EXTRA_SILENT, false);
            String message = intent.getStringExtra(RecordingService.EXTRA_MESSAGE);
            if (button != null) {
                if (recording) button.showRecording(seconds, audioLevel, silent);
                else button.showIdle(message);
            }
        }
    };

    @SuppressLint("UnspecifiedRegisterReceiverFlag")
    @Override public void onCreate() {
        super.onCreate();
        Notifications.ensureChannels(this);
        registerReceiver(
                stateReceiver,
                new IntentFilter(RecordingService.ACTION_STATE),
                "com.zheliu.pokecapsule.permission.INTERNAL",
                null);
    }

    @Override public int onStartCommand(Intent intent, int flags, int startId) {
        startForeground(NOTIFICATION_ID, notification());
        String action = intent == null ? "" : intent.getAction();
        if (ACTION_DISABLE.equals(action)) {
            getSharedPreferences("overlay", MODE_PRIVATE).edit().putBoolean("enabled", false).apply();
            stopSelf();
            return START_NOT_STICKY;
        }
        if (ACTION_HIDE.equals(action)) {
            stopSelf();
            return START_NOT_STICKY;
        }
        if (!Settings.canDrawOverlays(this)) {
            stopSelf();
            return START_NOT_STICKY;
        }
        getSharedPreferences("overlay", MODE_PRIVATE).edit().putBoolean("enabled", true).apply();
        if (button == null) showButton();
        else clampAndSave();
        return START_STICKY;
    }

    @Override public void onConfigurationChanged(Configuration newConfig) {
        super.onConfigurationChanged(newConfig);
        if (button != null) button.post(this::clampAndSave);
    }

    @Override public void onDestroy() {
        unregisterReceiver(stateReceiver);
        if (button != null && windowManager != null) windowManager.removeView(button);
        button = null;
        stopForeground(true);
        super.onDestroy();
    }

    @Override public IBinder onBind(Intent intent) {
        return null;
    }

    private void showButton() {
        windowManager = (WindowManager) getSystemService(WINDOW_SERVICE);
        if (windowManager == null) return;
        button = new CapsuleRecordButtonView(this);

        int type = Build.VERSION.SDK_INT >= 26
                ? WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY
                : WindowManager.LayoutParams.TYPE_PHONE;
        layout = new WindowManager.LayoutParams(
                dp(64), dp(64), type,
                WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                        | WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS,
                PixelFormat.TRANSLUCENT);
        layout.gravity = Gravity.TOP | Gravity.START;
        layout.x = getSharedPreferences("overlay", MODE_PRIVATE).getInt("x", dp(8));
        layout.y = getSharedPreferences("overlay", MODE_PRIVATE).getInt("y", dp(240));
        button.setOnTouchListener(this::onTouch);
        windowManager.addView(button, layout);
        button.post(this::clampAndSave);
    }

    private boolean onTouch(View view, MotionEvent event) {
        switch (event.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
                downX = event.getRawX();
                downY = event.getRawY();
                startX = layout.x;
                startY = layout.y;
                moved = false;
                return true;
            case MotionEvent.ACTION_MOVE:
                int dx = Math.round(event.getRawX() - downX);
                int dy = Math.round(event.getRawY() - downY);
                if (Math.abs(dx) > dp(4) || Math.abs(dy) > dp(4)) moved = true;
                layout.x = startX + dx;
                layout.y = Math.max(0, startY + dy);
                windowManager.updateViewLayout(button, layout);
                return true;
            case MotionEvent.ACTION_UP:
                if (moved) snapAndSave();
                else toggleRecording();
                return true;
            default:
                return false;
        }
    }

    private void snapAndSave() {
        int width = getResources().getDisplayMetrics().widthPixels;
        layout.x = layout.x + button.getWidth() / 2 < width / 2
                ? dp(8) : width - button.getWidth() - dp(8);
        clampAndSave();
    }

    private void clampAndSave() {
        if (button == null || windowManager == null || layout == null) return;
        int width = getResources().getDisplayMetrics().widthPixels;
        int height = getResources().getDisplayMetrics().heightPixels;
        int margin = dp(8);
        int buttonWidth = Math.max(button.getWidth(), dp(64));
        int buttonHeight = Math.max(button.getHeight(), dp(64));
        layout.x = Math.max(margin, Math.min(width - buttonWidth - margin, layout.x));
        layout.y = Math.max(0, Math.min(height - buttonHeight, layout.y));
        windowManager.updateViewLayout(button, layout);
        getSharedPreferences("overlay", MODE_PRIVATE).edit()
                .putInt("x", layout.x)
                .putInt("y", layout.y)
                .apply();
    }

    private void toggleRecording() {
        Intent intent = new Intent(this, RecordingService.class);
        intent.setAction(recording ? RecordingService.ACTION_STOP : RecordingService.ACTION_START);
        if (Build.VERSION.SDK_INT >= 26) startForegroundService(intent);
        else startService(intent);
    }

    private Notification notification() {
        PendingIntent open = PendingIntent.getActivity(
                this, 0, new Intent(this, MainActivity.class), PendingIntent.FLAG_UPDATE_CURRENT);
        Notification.Builder builder = Build.VERSION.SDK_INT >= 26
                ? new Notification.Builder(this, Notifications.CHANNEL_OVERLAY)
                : new Notification.Builder(this);
        return builder.setSmallIcon(android.R.drawable.ic_btn_speak_now)
                .setContentTitle("PokeCapsule 悬浮按钮")
                .setContentText("点按录音，拖动到屏幕边缘")
                .setContentIntent(open)
                .setOngoing(true)
                .build();
    }

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }
}
