package com.zheliu.pokecapsule.service;

import android.app.job.JobParameters;
import android.app.job.JobService;
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
            if (!TencentAsrConfig.isConfigured(this)) {
                reschedule = true;
                return;
            }
            List<CapsuleRecord> queued = queuedRecords(store.scan());
            if (queued.isEmpty() || stopped) return;
            try (RootWriteLock ignored = RootWriteLock.acquire(paths, "transcription")) {
                if (paths.isMaintenanceActive()) {
                    reschedule = true;
                    return;
                }
                for (CapsuleRecord record : queued) {
                    if (stopped) {
                        reschedule = true;
                        break;
                    }
                    process(store, record);
                }
            }
            if (!reschedule) reschedule = firstQueued(store.scan()) != null;
        } catch (Exception ignored) {
            reschedule = true;
        } finally {
            RUNNING.set(false);
            jobFinished(parameters, reschedule);
        }
    }

    private void process(CapsuleStore store, CapsuleRecord record) throws IOException {
        try {
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
                try {
                    TranscriptionGuard.requirePlausibleOutput(text, record.durationMs);
                } catch (IOException error) {
                    store.updateProcessing(record.directory, ProcessingState.FAILED,
                            "transcription-output", error.getMessage(), false);
                    return;
                }
                if (stopped) {
                    store.updateProcessing(record.directory, ProcessingState.QUEUED,
                            "transcription", "任务被系统暂停", false);
                    return;
                }
                AtomicFiles.writeUtf8(new File(record.directory, "raw.txt"), text + "\n");
                store.updateProcessing(record.directory, ProcessingState.RAW_READY,
                        null, null, false);
            } catch (TencentAsrClient.AsrException error) {
                boolean retryable = !"EmptyResult".equals(error.code);
                store.updateProcessing(
                        record.directory,
                        retryable ? ProcessingState.QUEUED : ProcessingState.FAILED,
                        "tencent-asr",
                        safeMessage(error),
                        false);
            } catch (Exception error) {
                store.updateProcessing(record.directory,
                        ProcessingState.QUEUED,
                        "tencent-asr",
                        stopped ? "任务被系统暂停" : safeMessage(error),
                        false);
            }
        } finally {
            LibraryChangeNotifier.notifyChanged(this);
        }
    }

    private static CapsuleRecord firstQueued(List<CapsuleRecord> records) {
        for (CapsuleRecord record : records) {
            if (record.status == ProcessingState.QUEUED) return record;
        }
        return null;
    }

    private static List<CapsuleRecord> queuedRecords(List<CapsuleRecord> records) {
        java.util.ArrayList<CapsuleRecord> queued = new java.util.ArrayList<>();
        for (CapsuleRecord record : records) {
            if (record.status == ProcessingState.QUEUED) queued.add(record);
        }
        return queued;
    }

    private static String safeMessage(Throwable error) {
        String message = error.getMessage();
        if (message == null || message.isEmpty()) message = error.getClass().getSimpleName();
        return message.length() > 500 ? message.substring(0, 500) : message;
    }
}
