package com.zheliu.pokecapsule.service;

import android.app.job.JobParameters;
import android.app.job.JobService;
import android.os.Debug;

import com.zheliu.pokecapsule.core.ProcessingState;
import com.zheliu.pokecapsule.model.CapsuleRecord;
import com.zheliu.pokecapsule.storage.AtomicFiles;
import com.zheliu.pokecapsule.storage.CapsuleStore;
import com.zheliu.pokecapsule.storage.PokePaths;
import com.zheliu.pokecapsule.storage.RootWriteLock;
import com.zheliu.pokecapsule.transcription.ModelVerifier;
import com.zheliu.pokecapsule.transcription.TranscriptionGuard;
import com.zheliu.pokecapsule.transcription.WhisperAdapter;
import com.zheliu.pokecapsule.transcription.WhisperCppAdapter;
import com.zheliu.pokecapsule.transcription.WhisperNative;

import java.io.File;
import java.io.IOException;
import java.util.List;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.atomic.AtomicBoolean;

public final class TranscriptionJobService extends JobService {
    private static final int MAX_PSS_KB = 450 * 1024;
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
        WhisperNative.cancelCurrent();
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
            ModelVerifier.Verification verification = ModelVerifier.verify(paths);
            if (!verification.valid) {
                reschedule = !verification.file.exists();
                if (verification.file.exists()) markFirstQueuedFailed(store, verification.message);
                return;
            }
            CapsuleRecord queued = firstQueued(store.scan());
            if (queued == null || stopped) return;
            try (RootWriteLock ignored = RootWriteLock.acquire(paths, "transcription")) {
                if (paths.isMaintenanceActive()) {
                    reschedule = true;
                    return;
                }
                process(store, verification.file, queued);
            }
            reschedule = firstQueued(store.scan()) != null;
        } catch (Exception ignored) {
            reschedule = true;
        } finally {
            RUNNING.set(false);
            jobFinished(parameters, reschedule);
        }
    }

    private void process(CapsuleStore store, File model, CapsuleRecord record) throws IOException {
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
            WhisperNative.prepareCurrent();
            if (stopped) {
                WhisperNative.cancelCurrent();
            }
            WhisperAdapter adapter = new WhisperCppAdapter();
            String text = adapter.transcribe(model, new File(record.directory, "audio.m4a"));
            TranscriptionGuard.requirePlausibleOutput(text, record.durationMs);
            if (stopped) {
                store.updateProcessing(record.directory, ProcessingState.QUEUED,
                        "transcription", "任务被系统暂停", false);
                return;
            }
            long pss = Debug.getPss();
            if (pss > MAX_PSS_KB) {
                store.updateProcessing(record.directory, ProcessingState.FAILED,
                        "transcription", "峰值内存门触发: " + pss + "KB", false);
                return;
            }
            AtomicFiles.writeUtf8(new File(record.directory, "raw.txt"), text + "\n");
            store.updateProcessing(record.directory, ProcessingState.RAW_READY,
                    null, null, false);
        } catch (IOException error) {
            store.updateProcessing(record.directory,
                    stopped ? ProcessingState.QUEUED : ProcessingState.FAILED,
                    "transcription",
                    stopped ? "任务被系统暂停" : safeMessage(error),
                    false);
        } catch (RuntimeException error) {
            store.updateProcessing(record.directory,
                    stopped ? ProcessingState.QUEUED : ProcessingState.FAILED,
                    "transcription",
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

    private static void markFirstQueuedFailed(CapsuleStore store, String message) throws IOException {
        CapsuleRecord queued = firstQueued(store.scan());
        if (queued != null) {
            store.updateProcessing(queued.directory, ProcessingState.FAILED,
                    "model", message, false);
        }
    }

    private static String safeMessage(Throwable error) {
        String message = error.getMessage();
        if (message == null || message.isEmpty()) message = error.getClass().getSimpleName();
        return message.length() > 500 ? message.substring(0, 500) : message;
    }
}
