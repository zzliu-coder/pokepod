#!/usr/bin/env python3
"""Production contract for non-blocking local recorder admission."""

from pathlib import Path


root = Path(__file__).parents[1]
app = (root / "firmware" / "PokePodAmoled" / "PokePodApp.cpp").read_text(
    encoding="utf-8"
)
state = (root / "firmware" / "PokePodAmoled" / "LocalRecordingStart.h").read_text(
    encoding="utf-8"
)

toggle = app[
    app.index("void toggleRecording() {") : app.index("void emitStatus() {")
]
advance = app[
    app.index("void advanceLocalRecordingStart() {") : app.index(
        "bool finishPendingCaptureStop() {"
    )
]
cleanup = app[
    app.index("void pollDeferredServiceCleanup() {") : app.index(
        "bool stopWirelessHold() {"
    )
]

assert "recorder.requestStart(" in toggle
assert "recorder.start(" not in toggle
assert "captureRuntime.start(" not in toggle
assert "localRecordingStart.begin(captureSessionId)" in toggle
assert "localRecordingStart.requestCancel()" in toggle
assert "recorder.pollStart(" in advance
assert "&localRecordingStartGate" in advance
assert advance.index("LocalRecordingStartAction::startCapture") < advance.index(
    "captureRuntime.start("
)
assert "recorder.abortCapture(usb.log())" in advance
assert "advanceLocalRecordingStart();" in cleanup
assert "localRecordingStart.active() || recorder.operationActive()" in app
assert "localRecordingStart.requestCancel();\n  safeShutdownQuiesce.request();" in app
assert "LocalRecordingStartAction::waiting" in state
assert "result == RecorderStartPollResult::started && !cancelRequested_" in state

print("PASS test_local_recording_start")
