package com.zheliu.pokecapsule.service;

import android.Manifest;
import android.app.Notification;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.os.Build;
import android.os.Environment;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.PowerManager;
import android.os.StatFs;
import android.os.SystemClock;

import com.zheliu.pokecapsule.core.Ids;
import com.zheliu.pokecapsule.core.AudioLevel;
import com.zheliu.pokecapsule.core.AudioFilePolicy;
import com.zheliu.pokecapsule.storage.CapsuleStore;
import com.zheliu.pokecapsule.storage.PokePaths;
import com.zheliu.pokecapsule.transcription.SentenceAudioPolicy;
import com.zheliu.pokecapsule.ui.MainActivity;

import java.io.File;

public final class RecordingService extends Service {
    public static final String ACTION_START = "com.zheliu.pokecapsule.RECORD_START";
    public static final String ACTION_STOP = "com.zheliu.pokecapsule.RECORD_STOP";
    public static final String ACTION_STATE = "com.zheliu.pokecapsule.RECORD_STATE";
    public static final String EXTRA_RECORDING = "recording";
    public static final String EXTRA_SECONDS_LEFT = "secondsLeft";
    public static final String EXTRA_MESSAGE = "message";
    public static final String EXTRA_AUDIO_LEVEL = "audioLevel";
    public static final String EXTRA_SILENT = "silent";

    private static final int NOTIFICATION_ID = 4202;
    private static final long DISPLAY_DURATION_MS = 60_000;
    private static final long MAX_DURATION_MS = SentenceAudioPolicy.SAFE_CAPTURE_DURATION_MS;
    private static final long MIN_FREE_BYTES = 5L * 1024 * 1024;

    private final Handler handler = new Handler(Looper.getMainLooper());
    private EnhancedAudioRecorder recorder;
    private File stagingDirectory;
    private long startedAt;
    private boolean recording;
    private int silentTicks;
    private long lastRecordedDurationMs;
    private PowerManager.WakeLock wakeLock;

    private final Runnable tick = new Runnable() {
        @Override public void run() {
            if (!recording) return;
            long elapsed = SystemClock.elapsedRealtime() - startedAt;
            int secondsLeft =
                    (int) Math.max(0, (DISPLAY_DURATION_MS - elapsed + 999) / 1000);
            int amplitude = 0;
            try {
                if (recorder != null) amplitude = recorder.getMaxAmplitude();
            } catch (RuntimeException ignored) {
                amplitude = 0;
            }
            int level = AudioLevel.fromAmplitude(amplitude);
            silentTicks = level == 0 ? silentTicks + 1 : 0;
            broadcast(true, secondsLeft, level, silentTicks >= 4, "");
            if (elapsed >= MAX_DURATION_MS) finishRecording();
            else handler.postDelayed(this, 500);
        }
    };

    @Override public void onCreate() {
        super.onCreate();
        Notifications.ensureChannels(this);
    }

    @Override public int onStartCommand(Intent intent, int flags, int startId) {
        startForeground(NOTIFICATION_ID, notification("准备录音"));
        String action = intent == null ? ACTION_START : intent.getAction();
        if (ACTION_STOP.equals(action)) finishRecording();
        else if (!recording) beginRecording();
        return START_NOT_STICKY;
    }

    @Override public IBinder onBind(Intent intent) {
        return null;
    }

    @Override public void onDestroy() {
        if (recording) finishRecording();
        releaseWakeLock();
        super.onDestroy();
    }

    private void beginRecording() {
        if (checkSelfPermission(Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED
                || checkSelfPermission(Manifest.permission.WRITE_EXTERNAL_STORAGE)
                != PackageManager.PERMISSION_GRANTED) {
            fail("请先授予麦克风和存储权限");
            return;
        }
        if (Environment.getExternalStorageState() == null
                || !Environment.MEDIA_MOUNTED.equals(Environment.getExternalStorageState())) {
            fail("共享存储当前不可写");
            return;
        }
        StatFs stats = new StatFs(Environment.getExternalStorageDirectory().getAbsolutePath());
        if (stats.getAvailableBytes() < MIN_FREE_BYTES) {
            fail("剩余空间不足 5MB");
            return;
        }

        CapsuleStore store = new CapsuleStore(new PokePaths());
        String id = Ids.newId();
        try {
            lastRecordedDurationMs = 0;
            stagingDirectory = store.beginRecording(id);
            File output = new File(stagingDirectory, AudioFilePolicy.ANDROID_AUDIO_FILE);
            File original = new File(stagingDirectory, "audio.original.wav");
            recorder = new EnhancedAudioRecorder(
                    output, original, DeviceCapabilities.current().eink);
            recorder.start();
            recording = true;
            silentTicks = 0;
            startedAt = SystemClock.elapsedRealtime();
            acquireWakeLock();
            handler.post(tick);
            startForeground(NOTIFICATION_ID, notification("录音中 · 最长 60 秒"));
            broadcast(true, 60, 0, false, "");
        } catch (Exception error) {
            releaseRecorder(false);
            fail("无法开始录音: " + safeMessage(error));
        }
    }

    private void finishRecording() {
        if (!recording) {
            stopForeground(true);
            stopSelf();
            return;
        }
        recording = false;
        handler.removeCallbacks(tick);
        boolean stoppedCleanly = releaseRecorder(true);
        long duration = lastRecordedDurationMs > 0
                ? lastRecordedDurationMs
                : SystemClock.elapsedRealtime() - startedAt;
        releaseWakeLock();
        if (!stoppedCleanly) {
            fail("录音停止异常；音频保留在暂存区");
            return;
        }
        try {
            CapsuleStore store = new CapsuleStore(new PokePaths());
            store.commitRecording(stagingDirectory, duration);
            LibraryChangeNotifier.notifyChanged(this);
            broadcast(false, 0, 0, false, "已保存到 Inbox");
        } catch (Exception error) {
            broadcast(false, 0, 0, false,
                    "保存失败，音频仍在暂存区: " + safeMessage(error));
        }
        stopForeground(true);
        stopSelf();
    }

    private boolean releaseRecorder(boolean stopFirst) {
        boolean clean = true;
        if (recorder != null) {
            EnhancedAudioRecorder current = recorder;
            try {
                if (stopFirst) current.stop();
                else current.abort();
            } catch (Exception error) {
                clean = false;
                current.abort();
            } finally {
                lastRecordedDurationMs = current.getRecordedDurationMs();
                recorder = null;
            }
        }
        return clean;
    }

    private void acquireWakeLock() {
        PowerManager manager = (PowerManager) getSystemService(POWER_SERVICE);
        if (manager == null) return;
        wakeLock = manager.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "PokeCapsule:recording");
        wakeLock.setReferenceCounted(false);
        wakeLock.acquire(MAX_DURATION_MS + 10_000);
    }

    private void releaseWakeLock() {
        if (wakeLock != null && wakeLock.isHeld()) wakeLock.release();
        wakeLock = null;
    }

    private Notification notification(String text) {
        Intent open = new Intent(this, MainActivity.class);
        PendingIntent pending = PendingIntent.getActivity(
                this, 0, open, PendingIntent.FLAG_UPDATE_CURRENT);
        Notification.Builder builder = Build.VERSION.SDK_INT >= 26
                ? new Notification.Builder(this, Notifications.CHANNEL_RECORDING)
                : new Notification.Builder(this);
        return builder.setSmallIcon(android.R.drawable.ic_btn_speak_now)
                .setContentTitle("PokeCapsule")
                .setContentText(text)
                .setContentIntent(pending)
                .setOngoing(recording)
                .build();
    }

    private void fail(String message) {
        broadcast(false, 0, 0, false, message);
        stopForeground(true);
        stopSelf();
    }

    private void broadcast(
            boolean active,
            int secondsLeft,
            int audioLevel,
            boolean silent,
            String message) {
        Intent state = new Intent(ACTION_STATE);
        state.setPackage(getPackageName());
        state.putExtra(EXTRA_RECORDING, active);
        state.putExtra(EXTRA_SECONDS_LEFT, secondsLeft);
        state.putExtra(EXTRA_AUDIO_LEVEL, audioLevel);
        state.putExtra(EXTRA_SILENT, silent);
        state.putExtra(EXTRA_MESSAGE, message);
        sendBroadcast(state);
    }

    private static String safeMessage(Throwable error) {
        String value = error.getMessage();
        return value == null || value.isEmpty() ? error.getClass().getSimpleName() : value;
    }
}
