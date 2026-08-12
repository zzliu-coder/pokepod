#!/usr/bin/env python3

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
FIRMWARE = ROOT / "firmware" / "PokePodAmoled"


def source(name: str) -> str:
    return (FIRMWARE / name).read_text(encoding="utf-8")


dashboard = source("Dashboard.cpp")
board = source("BoardServices.cpp")
main = source("PokePodApp.cpp")
recorder = source("WavRecorder.cpp") + source("WavRecorder.h")
capture = source("AudioCaptureService.h") + source("AudioCaptureRuntime.h")
voice = source("VoiceSessionController.h")
link = source("PokePodLinkService.cpp")
link_diagnostics = source("LinkDiagnostics.cpp")
audio = source("AudioFrontEnd.h")
conditioner = source("VoiceConditioner.h")
hiss_filter = source("TargetedHissFilter.h")

assert "formatUtcOffsetShort(record.createdAt.c_str()" in dashboard
assert "kChinaStandardTimeOffsetMinutes" in dashboard
assert "buildLocalDateTimeToUtcEpoch(__DATE__, __TIME__" in board
assert "setUtcEpoch(static_cast<time_t>(utcEpoch))" in board
assert "board.setUtcEpoch(synchronizedEpoch)" in main

assert "AudioFrontEnd audioFrontEnd_" not in recorder
assert "AudioFrontEnd audioFrontEnd_" in voice
assert "audioFrontEnd_.processStereo16" not in recorder
assert "audioFrontEnd_.processStereo16" in voice
assert "AudioFrontEnd frontEnd_" in capture
assert "frontEnd_.processStereo16" in capture
assert "AudioCaptureFrontEndSnapshot frontEndSnapshot() const" in capture
assert "recorder.observeAudioMetrics(snapshot.sessionId" in main
assert "AudioDecimator" not in recorder + voice
assert "kFirTaps = 79" in audio
assert '#include "VoiceConditioner.h"' in audio
assert "VoiceConditioner conditioner_" in audio
assert '#include "TargetedHissFilter.h"' in conditioner
assert "TargetedHissFilter hissFilter_" in conditioner
assert "kTaps = 67" in hiss_filter
assert "5.05-6.95 kHz ideal band-stop" in hiss_filter
assert "kHighPassFeedbackQ15" in conditioner
assert "kMaximumGainQ12 = 6 * 4096" in conditioner
assert "kLimiter = 30000" in conditioner
assert "kClosedGateGainQ12 = 128" in conditioner
assert "audio_frontend_channel" in main
assert "audio_frontend_channel" in link_diagnostics
assert "audio_frontend_noise_floor" in main
assert "audio_frontend_noise_floor" in link_diagnostics
assert "audio_frontend_suppressed_samples" in main
assert "audio_frontend_suppressed_samples" in link_diagnostics

assert "PageTransition pageTransition_" in source("Dashboard.h")
assert "dashboard.advancePageTransition(now)" in main
assert "dashboard.pageTransitionActive()" in main

print("PASS test_time_audio_motion_contract")
