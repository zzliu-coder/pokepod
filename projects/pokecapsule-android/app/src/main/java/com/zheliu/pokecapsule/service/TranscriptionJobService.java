package com.zheliu.pokecapsule.service;

import android.app.job.JobParameters;
import android.app.job.JobService;
import android.os.BatteryManager;
import com.zheliu.pokecapsule.core.ProcessingState;
import com.zheliu.pokecapsule.model.CapsuleRecord;
import com.zheliu.pokecapsule.storage.AtomicFiles;
import com.zheliu.pokecapsule.storage.CapsuleStore;
import com.zheliu.pokecapsule.storage.PokePaths;
import com.zheliu.pokecapsule.storage.RootWriteLock;
import com.zheliu.pokecapsule.transcription.TranscriptionGuard;
import com.zheliu.pokecapsule.transcription.TencentAsrClient;
import com.zheliu.pokecapsule.transcription.TencentAsrConfig;

import java.io.File;
import java.io.IOException;
import java.util.List;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.atomic.AtomicBoolean;

public final class TranscriptionJobService extends JobService {
    private static final AtomicBoolean RUNNING = new AtomicBoolean(false);
    private final ExecutorService executor = Executors.newSingleThreadExecutor();
    private volatile boolean stopped;

    @Override public boolean onStartJob(JobParameters parameters) {
        if (!RUNNING.compareAndSet(false, true)) return false;
        stopped = false;
        executor.execute(() -> runOne(parameters));
        return true;
    }

    @Override public boolean onStopJob(JobParameters parameters) {
        stopped = true;
        return true;
    }

    @Override public void onDestroy() {
        executor.shutdownNow();
        super.onDestroy();
    }

    private void runOne(JobParameters parameters) {
        boolean reschedule = false;
        try {
            PokePaths paths = new PokePaths();
            CapsuleStore store = new CapsuleStore(paths);
            paths.ensureBase();
            boolean manual = parameters.getExtras().getBoolean("manual", false);
            if (!manual && !BatteryPolicy.allowsAutomatic(batteryPercent())) {
                reschedule = true;
                return;
            }
            if (!TencentAsrConfig.isConfigured(this)) {
                reschedule = true;
                return;
            }
            CapsuleRecord queued = firstQueued(store.scan());
            if (queued == null || stopped) return;
            try (RootWriteLock ignored = RootWriteLock.acquire(paths, "transcription")) {
                if (paths.isMaintenanceActive()) {
                    reschedule = true;
                    return;
                }
                process(store, queued);
            }
            reschedule = firstQueued(store.scan()) != null;
        } catch (Exception ignored) {
            reschedule = true;
        } finally {
            RUNNING.set(false);
            jobFinished(parameters, reschedule);
        }
    }

    private void process(CapsuleStore store, CapsuleRecord record) throws IOException {
        try {
            TranscriptionGuard.requireTranscribableDuration(record.durationMs);
        } catch (IOException error) {
            store.updateProcessing(record.directory, ProcessingState.FAILED,
                    "audio", error.getMessage(), false);
            return;
        }
        store.updateProcessing(record.directory, ProcessingState.TRANSCRIBING,
                null, null, true);
        try {
            if (stopped) {
                store.updateProcessing(record.directory, ProcessingState.QUEUED,
                        "transcription", "任务被系统暂停", false);
                return;
            }
            TencentAsrConfig credentials = TencentAsrConfig.load(this);
            String text = new TencentAsrClient().transcribe(
                    new File(record.directory, "audio.m4a"), credentials);
            TranscriptionGuard.requirePlausibleOutput(text, record.durationMs);
            if (stopped) {
                store.updateProcessing(record.directory, ProcessingState.QUEUED,
                        "transcription", "任务被系统暂停", false);
                return;
            }
            AtomicFiles.writeUtf8(new File(record.directory, "raw.txt"), text + "\n");
            store.updateProcessing(record.directory, ProcessingState.RAW_READY,
                    null, null, false);
        } catch (TencentAsrClient.AsrException error) {
            store.updateProcessing(record.directory, ProcessingState.QUEUED,
                    "tencent-asr", safeMessage(error), false);
        } catch (Exception error) {
            store.updateProcessing(record.directory,
                    ProcessingState.QUEUED,
                    "tencent-asr",
                    stopped ? "任务被系统暂停" : safeMessage(error),
                    false);
        }
    }

    private static CapsuleRecord firstQueued(List<CapsuleRecord> records) {
        for (CapsuleRecord record : records) {
            if (record.status == ProcessingState.QUEUED) return record;
        }
        return null;
    }

    private int batteryPercent() {
        BatteryManager manager = (BatteryManager) getSystemService(BATTERY_SERVICE);
        return manager == null
                ? -1
                : manager.getIntProperty(BatteryManager.BATTERY_PROPERTY_CAPACITY);
    }

    private static String safeMessage(Throwable error) {
        String message = error.getMessage();
        if (message == null || message.isEmpty()) message = error.getClass().getSimpleName();
        return message.length() > 500 ? message.substring(0, 500) : message;
    }
}
