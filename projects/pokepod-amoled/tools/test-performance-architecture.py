#!/usr/bin/env python3

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FIRMWARE = ROOT / "firmware" / "PokePodAmoled"


def require(source: str, needle: str, label: str) -> None:
    if needle not in source:
        raise SystemExit(f"FAIL performance_architecture: {label}")


library = (FIRMWARE / "CapsuleLibrary.cpp").read_text(encoding="utf-8")
app = (FIRMWARE / "PokePodApp.cpp").read_text(encoding="utf-8")
dashboard = (FIRMWARE / "Dashboard.cpp").read_text(encoding="utf-8")
audio = (FIRMWARE / "AudioPipeline.cpp").read_text(encoding="utf-8")
renderer = (FIRMWARE / "ChineseRenderer.cpp").read_text(encoding="utf-8")

for operation in (
    "markTranscribing", "commitRawText", "markFailure", "markRetryable",
    "requeue", "toggleFavorite", "archive", "unarchive", "trash", "restore",
):
    start = library.index(f"CapsuleLibrary::{operation}")
    next_method = library.find("\n}\n\n", start)
    body = library[start:next_method]
    if "&& scan()" in body or "return scan();" in body:
        raise SystemExit(
            f"FAIL performance_architecture: {operation} still forces full scan"
        )

require(app, "capsuleLibrary.includeInboxCapsule(recorder.capsuleId())",
        "recording completion still scans every capsule")
require(library, "deferredPublish_.begin();",
        "batch mutations do not coalesce visible-index publication")
require(library, "file.read(chunk, request)",
        "small text files are still read one byte at a time")
require(dashboard, "UiSignatureBuilder value;",
        "frame signatures still allocate concatenated Strings")
require(dashboard, "rowPresentation(*record)",
        "visible capsule row presentation is not cached")
require(dashboard, "std::memmove(base + reuse.destinationRow",
        "scroll composition does not reuse prior pixels")
require(audio, "playbackReadSize(playbackFileRemaining_)",
        "playback does not use read-ahead")
require(audio, "playbackFeedSize(",
        "playback read-ahead is not split into bounded I2S feeds")
require(renderer, "sdCacheLookup_[slot]",
        "SD glyph cache lacks direct hot lookup")

print("PASS performance_architecture")
