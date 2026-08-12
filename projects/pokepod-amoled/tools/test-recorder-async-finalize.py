#!/usr/bin/env python3
"""Production contract for local recording asynchronous finalization."""

from pathlib import Path


root = Path(__file__).parents[1]
firmware = root / "firmware" / "PokePodAmoled"
wav_h = (firmware / "WavRecorder.h").read_text(encoding="utf-8")
wav = (firmware / "WavRecorder.cpp").read_text(encoding="utf-8")
app = (firmware / "PokePodApp.cpp").read_text(encoding="utf-8")
capture = (firmware / "AudioCaptureRuntime.h").read_text(encoding="utf-8")
storage_queue = (firmware / "RecorderStorageQueue.h").read_text(encoding="utf-8")
dispatcher = (firmware / "AudioCaptureDispatcher.h").read_text(encoding="utf-8")

assert "CapsuleTransactionRunner transactionRunner_" in wav_h
assert "RecorderOperationOwner operationOwner() const" in wav_h
assert "bool requestStart(Print &log" in wav_h
assert "RecorderStartPollResult pollStart" in wav_h
assert "bool pollFinalize(Print &log" in wav_h
assert "commitPreparedFile" not in wav
assert "recording_finalize_pending" in wav
assert "startState_.poll(" in wav
assert "capsuleTransactionPermitted(gate, nowMs)" in wav
assert "startAudioAndProcessing" in wav
assert 'directory_ + "/recording.failed.chk"' in wav
assert "transactionRunner_.poll(nowMs, gate)" in wav
assert "static constexpr size_t kRingFrames = 6" in capture
assert "kRecorderStorageQueueFrames = 128" in storage_queue
assert "MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT" in wav
assert '"pokepod_recorder_storage"' in wav
assert "storageQueue_.push(data, length)" in wav
assert "std::atomic<RecorderStopReason> automaticStopReason_" in wav_h
assert "std::atomic<bool> storageAbortRequested_" in wav_h
assert "bool append(const uint8_t *data" not in wav_h
assert "bool WavRecorder::append(const uint8_t *data" not in wav
assert "AudioFrontEnd audioFrontEnd_" not in wav_h
assert "audioFrontEnd_.processStereo16" not in wav
assert "AudioFrontEndMetrics audioMetrics_" in wav_h
assert "#if defined(ARDUINO_ARCH_ESP32)\n  if (!recording_ || data == nullptr || length == 0" in wav
assert "pollPeriodicCheckpoint" in wav
public_poll = wav[
    wav.index("bool WavRecorder::pollFinalize(Print"):
    wav.index("void WavRecorder::resetSessionState")
]
assert "periodicCheckpointPhase_" not in public_poll
assert "transactionRunner_.active()" not in wav_h[
    wav_h.index("#if defined(ARDUINO_ARCH_ESP32)",
                wav_h.index("bool operationActive() const")):
    wav_h.index("#else", wav_h.index("bool operationActive() const"))
]
assert "storageStartRequested_.store(true" in wav
assert "xSemaphoreTake(storageStartAck_" in wav
assert "startStorageSession(*log)" in wav
start_internal = wav[
    wav.index("bool WavRecorder::startInternal"):
    wav.index("bool WavRecorder::startStorageSession")
]
assert "StorageCoordinator::instance().reserve" not in start_internal
storage_start = wav[
    wav.index("bool WavRecorder::startStorageSession"):
    wav.index("bool WavRecorder::appendMono16")
]
assert "StorageCoordinator::instance().reserve" in storage_start
assert "fs_->open(partialPath_, FILE_WRITE)" in storage_start
append_start = wav.index("bool WavRecorder::appendMonoBytes")
append_end = wav.index("bool WavRecorder::storageAppendMonoBytes", append_start)
assert "persistCheckpoint" not in wav[append_start:append_end]
capture_failure = wav[
    wav.index("void WavRecorder::reportCaptureFailure"):
    wav.index("bool WavRecorder::finalizeFailure")
]
assert "captureFailureCode_.compare_exchange_strong" in capture_failure
assert "storageAbortRequested_.store(true" in capture_failure
assert "recording_.store(false" not in capture_failure
assert "RecorderFailureStage::storageQueueOverflow" in wav
assert "captureFailureLatched()" in wav[
    wav.index("void WavRecorder::completeFinalize"):
    wav.index("bool WavRecorder::pollBootRecovery")
]
assert "while (source.pop(frame))" in dispatcher
assert "reportCaptureFailure(" in dispatcher


assert "RecorderOperationOwner::localApp" in app
assert "recorder.ownedBy(RecorderOperationOwner::localApp)" in app
assert "pendingRecorderFinalize && !recorder.operationActive()" in app
assert "captureRouter.release(AudioCaptureOwner::localCapsule)" in app
assert "AudioCaptureDispatcher captureDispatcher;" in app
dispatch_loop = app[
    app.index("const AudioCaptureDispatchResult dispatch = drainCapturedAudio(now);"):
    app.index("if (recorder.ownedBy(",
              app.index("const AudioCaptureDispatchResult dispatch = drainCapturedAudio(now);"))
]
assert "recorder.abortCapture" not in dispatch_loop
assert "requestCaptureStop(PendingCaptureStop::localCapsule" in dispatch_loop
assert "failedRecorderOwner == RecorderOperationOwner::localApp" in dispatch_loop

finish_stop = app[app.index("bool finishPendingCaptureStop() {"):
                  app.index("bool requestCaptureStop(",
                            app.index("bool finishPendingCaptureStop() {"))]
assert finish_stop.index("observeCaptureMetrics()") < finish_stop.index(
    "recorder.abortCapture(usb.log())")
assert "录音已中断" in app
assert "recorder.operationActive() || pendingRecorderFinalize" in app
assert "recorder.pollCleanup" not in app
assert "recorder.recoverInterrupted" not in app
assert app.index("if (safeShutdownQuiesce.pending())") < app.index(
    "if (linkService.receivingBinary())"
)

owner_block_start = app.index(
    "if (recorder.recording() &&\n"
    "      recorder.ownedBy(RecorderOperationOwner::localApp)) {"
)
owner_block_end = app.index("\n  }", owner_block_start) + len("\n  }")
owner_block = app[owner_block_start:owner_block_end]
assert "recorder.pollFinalize(usb.log(), millis(), nullptr)" in owner_block

print("PASS test_recorder_async_finalize")
