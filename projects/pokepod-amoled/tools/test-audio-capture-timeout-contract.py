#!/usr/bin/env python3
"""Keep the fixed ESP_I2S timeout truthful from driver to shutdown."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
FIRMWARE = ROOT / "firmware" / "PokePodAmoled"


def read(name: str) -> str:
    return (FIRMWARE / name).read_text(encoding="utf-8")


timing = read("AudioCaptureTiming.h")
service = read("AudioCaptureService.h")
runtime_h = read("AudioCaptureRuntime.h")
runtime_cpp = read("AudioCaptureRuntime.cpp")
pipeline = read("AudioPipeline.cpp")
test = (ROOT / "firmware" / "tests" / "test_audio_capture_reliability.cpp").read_text(
    encoding="utf-8"
)

assert "constexpr uint32_t kAudioCaptureReadTimeoutMs = 50U;" in timing
assert "kAudioCaptureReadTimeoutMs * kAudioCaptureStopReadWindows" in timing
assert "kAudioCaptureStopTimeoutMs" in timing
assert "i2s_.setTimeout(kAudioCaptureReadTimeoutMs);" in pipeline
assert "i2s_.setTimeout(50)" not in pipeline
assert "kStopTimeoutMs = kAudioCaptureStopTimeoutMs" in runtime_h
assert r'\"read_timeout_ms\":%u' in runtime_cpp
assert r'\"stop_timeout_ms\":%u' in runtime_cpp
assert "uint32_t timeoutMs" not in service
assert "uint32_t timeoutMs" not in runtime_h
assert "uint32_t timeoutMs" not in runtime_cpp
assert "(void)timeoutMs" not in runtime_cpp
assert "readStereo48(\n        raw_ + rawUsed_, sizeof(raw_) - rawUsed_)" in service
assert "kAudioCaptureReadTimeoutMs == 50U" in test
assert "kAudioCaptureStopReadWindows" in test
assert "sourceEarlyZero" in service
assert "earlyZeroReads_" in service
assert "cycle == AudioCaptureCycleResult::sourceEarlyZero" in runtime_cpp
assert "taskYIELD()" in runtime_cpp
print("PASS test_audio_capture_timeout_contract")
