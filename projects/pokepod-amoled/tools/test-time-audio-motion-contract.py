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
assert "kHighPassFeedbackQ15" in audio
assert "kMaximumGainQ12" in audio
assert "kLimiter = 30000" in audio
assert "audio_frontend_channel" in main
assert "audio_frontend_channel" in link

assert "PageTransition pageTransition_" in source("Dashboard.h")
assert "dashboard.advancePageTransition(now)" in main
assert "dashboard.pageTransitionActive()" in main

print("PASS test_time_audio_motion_contract")
