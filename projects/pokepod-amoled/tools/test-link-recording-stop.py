#!/usr/bin/env python3

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CPP = (ROOT / "firmware/PokePodAmoled/PokePodLinkService.cpp").read_text()
HEADER = (ROOT / "firmware/PokePodAmoled/PokePodLinkService.h").read_text()
STATE = (ROOT / "firmware/PokePodAmoled/LinkRecordingStop.h").read_text()

assert "requestLinkRecordingStop" in HEADER
assert "advanceLinkRecordingStop" in HEADER
assert "LinkRecordingStop linkRecordingStop_" in HEADER
assert "stopLinkRecording" not in HEADER

disconnect = CPP[CPP.index("void PokePodLinkService::disconnect()"):
                 CPP.index("void PokePodLinkService::pollDeferredCleanup()")]
assert "suppressResponseAndAbort" in disconnect
assert "requestLinkRecordingStop(0, false, false)" in disconnect
assert "captureRouter_->release" not in disconnect

advance = CPP[CPP.index("void PokePodLinkService::advanceLinkRecordingStop()"):
              CPP.index("bool PokePodLinkService::transferPermitted()")]
assert "captureRuntime_->pollFinalize" in advance
assert "captureRuntime_->running()" in advance
assert advance.index("captureRuntime_->running()") < advance.index(
    "captureRouter_->release")
assert "recorder_->cleanupPending()" in advance
assert advance.index("recorder_->cleanupPending()") < advance.index(
    "captureRouter_->release")
assert "rememberCompleted(requestId)" in advance

assert "awaitCaptureFinalize" in STATE
assert "awaitRecorderCleanup" in STATE
assert "suppressResponseAndAbort" in STATE

print("PASS link_recording_stop_contract")
