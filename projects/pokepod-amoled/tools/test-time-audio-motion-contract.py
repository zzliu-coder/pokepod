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
voice = source("VoiceSessionController.h")
link = source("PokePodLinkService.cpp")
audio = source("AudioFrontEnd.h")
conditioner = source("VoiceConditioner.h")

assert "formatUtcOffsetShort(record.createdAt.c_str()" in dashboard
assert "kChinaStandardTimeOffsetMinutes" in dashboard
assert "buildLocalDateTimeToUtcEpoch(__DATE__, __TIME__" in board
assert "setUtcEpoch(static_cast<time_t>(utcEpoch))" in board
assert "board.setUtcEpoch(synchronizedEpoch)" in main

assert "AudioFrontEnd audioFrontEnd_" in recorder
assert "AudioFrontEnd audioFrontEnd_" in voice
assert "audioFrontEnd_.processStereo16" in recorder
assert "audioFrontEnd_.processStereo16" in voice
assert "AudioDecimator" not in recorder + voice
assert "kFirTaps = 79" in audio
assert '#include "VoiceConditioner.h"' in audio
assert "VoiceConditioner conditioner_" in audio
assert "kSpeechFirTaps = 31" in conditioner
assert "kHighPassFeedbackQ15" in conditioner
assert "kMaximumGainQ12 = 6 * 4096" in conditioner
assert "kLimiter = 30000" in conditioner
assert "kClosedGateGainQ12 = 128" in conditioner
assert "audio_frontend_channel" in main
assert "audio_frontend_channel" in link
assert "audio_frontend_noise_floor" in main
assert "audio_frontend_noise_floor" in link
assert "audio_frontend_suppressed_samples" in main
assert "audio_frontend_suppressed_samples" in link

assert "PageTransition pageTransition_" in source("Dashboard.h")
assert "dashboard.advancePageTransition(now)" in main
assert "dashboard.pageTransitionActive()" in main

print("PASS test_time_audio_motion_contract")
