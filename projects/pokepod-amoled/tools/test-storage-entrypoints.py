#!/usr/bin/env python3
"""Contract: every product SD entrypoint participates in one coordinator."""

from pathlib import Path


root = Path(__file__).parents[1]
firmware = root / "firmware" / "PokePodAmoled"
coordinator = (firmware / "StorageCoordinator.h").read_text(encoding="utf-8")
audio_h = (firmware / "AudioPipeline.h").read_text(encoding="utf-8")
audio = (firmware / "AudioPipeline.cpp").read_text(encoding="utf-8")
font = (firmware / "ChineseRenderer.cpp").read_text(encoding="utf-8")

assert "audioPlayback" in coordinator
assert "fontRead" in coordinator
assert "StorageReservation playbackReservation_" in audio_h
assert "StorageOwner::audioPlayback, StorageAccess::read" in audio
assert "playbackReservation_ = std::move(storage)" in audio
assert "playbackReservation_.release()" in audio
assert audio.count("StorageOwner::audioPlayback") >= 5
assert font.count("StorageOwner::fontRead") >= 2
assert "if (!fontIo) return false" in font

print("PASS test_storage_entrypoints")
