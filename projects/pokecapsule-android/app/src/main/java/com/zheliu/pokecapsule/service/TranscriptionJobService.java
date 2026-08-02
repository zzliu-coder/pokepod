package com.zheliu.pokecapsule.service;

import android.app.job.JobParameters;
import android.app.job.JobService;

import com.zheliu.pokecapsule.PokeCapsuleApp;
import com.zheliu.pokecapsule.core.ProcessingState;
import com.zheliu.pokecapsule.model.CapsuleRecord;
import com.zheliu.pokecapsule.storage.AtomicFiles;
import com.zheliu.pokecapsule.storage.CapsuleStore;
import com.zheliu.pokecapsule.storage.DiagnosticLog;
import com.zheliu.pokecapsule.storage.PokePaths;
import com.zheliu.pokecapsule.storage.RootWriteLock;
import com.zheliu.pokecapsule.transcription.SentenceAudioPreparer;
import com.zheliu.pokecapsule.transcription.TencentAsrClient;
import com.zheliu.pokecapsule.transcription.TencentAsrConfig;
import com.zheliu.pokecapsule.transcription.TranscriptionErrorText;
import com.zheliu.pokecapsule.transcription.TranscriptionGuard;

import org.json.JSONException;

import java.io.File;
import java.io.IOException;
import java.net.ConnectException;
import java.net.SocketTimeoutException;
import java.net.UnknownHostException;
import java.util.List;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import java.util.concurrent.atomic.AtomicBoolean;

import javax.net.ssl.SSLException;

public final class TranscriptionJobService extends JobService {
    private static final int MAX_TRANSIENT_ATTEMPTS = 3;

    private final ExecutorService executor = Executors.newSingleThreadExecutor();
    private final ConcurrentHashMap<Integer, RunState> activeRuns = new ConcurrentHashMap<>();

    @Override public boolean onStartJob(JobParameters parameters) {
        RunState state = new RunState(parameters);
        if (activeRuns.putIfAbsent(parameters.getJobId(), state) != null) return false;
        state.future = executor.submit(() -> runOne(state));
        if (state.stopped.get() && !state.started.get()) {
            Future<?> work = state.future;
            if (work != null && work.cancel(true)) finishRun(state, false);
        }
        return true;
    }

    @Override public boolean onStopJob(JobParameters parameters) {
        RunState state = activeRuns.get(parameters.getJobId());
        if (state == null) return false;
        state.stopped.set(true);
        Future<?> work = state.future;
        if (work != null && work.cancel(true) && !state.started.get()) {
            finishRun(state, false);
        }
        return true;
    }

    @Override public void onDestroy() {
        for (RunState state : activeRuns.values()) {
            state.stopped.set(true);
            Future<?> work = state.future;
            if (work != null && work.cancel(true) && !state.started.get()) {
                finishRun(state, false);
            }
        }
        executor.shutdownNow();
        super.onDestroy();
    }

    private void runOne(RunState state) {
        if (!state.started.compareAndSet(false, true)) return;
        boolean reschedule = false;
        try {
            if (!PokeCapsuleApp.awaitStartupReady(30_000)) {
                reschedule = true;
                return;
            }
            if (state.stopped.get()) return;
            PokePaths paths = new PokePaths();
            CapsuleStore store = new CapsuleStore(paths);
            paths.ensureBase();
            if (!TencentAsrConfig.isConfigured(this) || state.stopped.get()) return;
            if (paths.isMaintenanceActive()) {
                reschedule = true;
                return;
            }
            RootWriteLock lock;
            try {
                lock = RootWriteLock.acquire(paths, "transcription");
            } catch (IOException error) {
                if ("设备正在维护或执行其他写操作".equals(error.getMessage())) {
                    reschedule = true;
                    return;
                }
                throw error;
            }
            try (RootWriteLock ignored = lock) {
                if (paths.isMaintenanceActive()) {
                    reschedule = true;
                    return;
                }
                List<CapsuleRecord> queued = queuedRecords(store.scan());
                if (queued.isEmpty()) return;
                for (CapsuleRecord record : queued) {
                    if (state.stopped.get()) {
                        reschedule = true;
                        break;
                    }
                    process(store, record, state);
                }
            }
            if (!reschedule) reschedule = firstQueued(store.scan()) != null;
        } catch (Exception error) {
            DiagnosticLog.record(
                    this, "transcription-job", error.getClass().getSimpleName(), error);
            reschedule = false;
        } finally {
            finishRun(state, reschedule);
        }
    }

    private void finishRun(RunState state, boolean reschedule) {
        if (!state.finished.compareAndSet(false, true)) return;
        activeRuns.remove(state.parameters.getJobId(), state);
        if (!state.stopped.get()) jobFinished(state.parameters, reschedule);
    }

    private void process(CapsuleStore store, CapsuleRecord record, RunState state)
            throws IOException {
        int attemptNumber = CapsuleStore.readJson(
                new File(record.directory, "processing.json")).optInt("attempts", 0) + 1;
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
                if (state.stopped.get()) {
                    pause(store, record);
                    return;
                }
                TencentAsrConfig credentials = TencentAsrConfig.load(this);
                SentenceAudioPreparer.PreparedAudio prepared;
                try {
                    prepared = SentenceAudioPreparer.prepare(
                            new File(record.directory, "audio.m4a"),
                            record.durationMs,
                            new File(getCacheDir(), "asr"));
                } catch (IOException error) {
                    store.updateProcessing(
                            record.directory,
                            ProcessingState.FAILED,
                            "audio",
                            error.getMessage(),
                            false);
                    return;
                }
                if (state.stopped.get()) {
                    prepared.close();
                    pause(store, record);
                    return;
                }
                String text;
                try (SentenceAudioPreparer.PreparedAudio ignored = prepared) {
                    text = new TencentAsrClient().transcribe(prepared.file, credentials);
                }
                try {
                    TranscriptionGuard.requirePlausibleOutput(text, record.durationMs);
                } catch (IOException error) {
                    store.updateProcessing(record.directory, ProcessingState.FAILED,
                            "transcription-output", error.getMessage(), false);
                    return;
                }
                if (state.stopped.get()) {
                    pause(store, record);
                    return;
                }
                AtomicFiles.writeUtf8(new File(record.directory, "raw.txt"), text + "\n");
                store.updateProcessing(record.directory, ProcessingState.RAW_READY,
                        null, null, false);
            } catch (TencentAsrClient.AsrException error) {
                if (state.stopped.get()) {
                    pause(store, record);
                    return;
                }
                DiagnosticLog.record(this, "tencent-asr", error.code, error);
                boolean retryable = TranscriptionErrorText.isRetryable(error.code)
                        && attemptNumber < MAX_TRANSIENT_ATTEMPTS;
                store.updateProcessing(
                        record.directory,
                        retryable ? ProcessingState.QUEUED : ProcessingState.FAILED,
                        "tencent-asr",
                        retryable
                                ? TranscriptionErrorText.userMessage(error.code)
                                : exhaustedMessage(error.code),
                        false);
            } catch (Exception error) {
                DiagnosticLog.record(
                        this, "transcription", error.getClass().getSimpleName(), error);
                boolean retryable = !state.stopped.get()
                        && isTransient(error)
                        && attemptNumber < MAX_TRANSIENT_ATTEMPTS;
                store.updateProcessing(
                        record.directory,
                        state.stopped.get() || retryable
                                ? ProcessingState.QUEUED
                                : ProcessingState.FAILED,
                        "transcription",
                        state.stopped.get()
                                ? "任务被系统暂停"
                                : retryable
                                        ? "网络或转写服务暂时不可用，稍后会自动重试"
                                        : deterministicFailureMessage(error),
                        false);
            }
        } finally {
            LibraryChangeNotifier.notifyChanged(this);
        }
    }

    private static void pause(CapsuleStore store, CapsuleRecord record) throws IOException {
        store.updateProcessing(record.directory, ProcessingState.QUEUED,
                "transcription", "任务被系统暂停", false);
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

    private static boolean isTransient(Throwable error) {
        Throwable current = error;
        while (current != null) {
            if (current instanceof SocketTimeoutException
                    || current instanceof ConnectException
                    || current instanceof UnknownHostException
                    || current instanceof SSLException
                    || current instanceof JSONException) {
                return true;
            }
            current = current.getCause();
        }
        return false;
    }

    private static String exhaustedMessage(String code) {
        if (!TranscriptionErrorText.isRetryable(code)) {
            return TranscriptionErrorText.userMessage(code);
        }
        return "转写服务连续失败 3 次，已停止自动重试；原录音已保留";
    }

    private static String deterministicFailureMessage(Throwable error) {
        if (error instanceof IllegalArgumentException) {
            String message = error.getMessage();
            return message == null || message.isEmpty()
                    ? "这段录音无法提交转写；原录音已保留"
                    : message;
        }
        if (error instanceof IOException) {
            return "本地音频或文字文件处理失败；原录音已保留";
        }
        return "转写处理失败，已停止自动重试；原录音已保留";
    }

    private static final class RunState {
        final JobParameters parameters;
        final AtomicBoolean started = new AtomicBoolean(false);
        final AtomicBoolean stopped = new AtomicBoolean(false);
        final AtomicBoolean finished = new AtomicBoolean(false);
        volatile Future<?> future;

        RunState(JobParameters parameters) {
            this.parameters = parameters;
        }
    }
}
