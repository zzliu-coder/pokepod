#!/usr/bin/env python3
"""Production contract for local recording asynchronous finalization."""

from pathlib import Path


root = Path(__file__).parents[1]
firmware = root / "firmware" / "PokePodAmoled"
wav_h = (firmware / "WavRecorder.h").read_text(encoding="utf-8")
wav = (firmware / "WavRecorder.cpp").read_text(encoding="utf-8")
app = (firmware / "PokePodApp.cpp").read_text(encoding="utf-8")
capture = (firmware / "AudioCaptureRuntime.h").read_text(encoding="utf-8")

assert "CapsuleTransactionRunner transactionRunner_" in wav_h
assert "RecorderOperationOwner operationOwner() const" in wav_h
assert "bool pollFinalize(Print &log" in wav_h
assert "commitPreparedFile" not in wav
assert "recording_finalize_pending" in wav
assert "startAudioAndProcessing" in wav
assert 'directory_ + "/recording.failed.chk"' in wav
assert "transactionRunner_.poll(nowMs, gate)" in wav
assert "static constexpr size_t kRingFrames = 64" in capture

assert "RecorderOperationOwner::localApp" in app
assert "recorder.ownedBy(RecorderOperationOwner::localApp)" in app
assert "pendingRecorderFinalize && !recorder.operationActive()" in app
assert "captureRouter.release(AudioCaptureOwner::localCapsule)" in app
assert "recorder.operationActive() || pendingRecorderFinalize" in app
assert "recorder.pollCleanup" not in app
assert "recorder.recoverInterrupted" not in app
assert app.index("if (safeShutdownQuiesce.pending())") < app.index(
    "if (linkService.receivingBinary())"
)

owner_gate = app.index("recorder.ownedBy(RecorderOperationOwner::localApp)")
poll = app.index("recorder.pollFinalize(usb.log(), millis(), nullptr)",
                 owner_gate)
assert poll - owner_gate < 180

print("PASS test_recorder_async_finalize")
