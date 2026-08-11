#!/usr/bin/env python3

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FIRMWARE = ROOT / "firmware" / "PokePodAmoled"


def require(source: str, needle: str, label: str) -> None:
    if needle not in source:
        raise SystemExit(f"FAIL performance_architecture: {label}")


library = (FIRMWARE / "CapsuleLibrary.cpp").read_text(encoding="utf-8")
library_header = (FIRMWARE / "CapsuleLibrary.h").read_text(encoding="utf-8")
index_policy = (FIRMWARE / "CapsuleIndexPolicy.h").read_text(encoding="utf-8")
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
if "kMaxCapsulesOnDevice = 96" in library:
    raise SystemExit("FAIL performance_architecture: filesystem scan still truncates at 96")
scan_start = library.index("void CapsuleLibrary::scanFolder")
scan_end = library.index("bool CapsuleLibrary::readRecord", scan_start)
if "readBestText" in library[scan_start:scan_end]:
    raise SystemExit("FAIL performance_architecture: scan eagerly reads preview text")
read_start = library.index("bool CapsuleLibrary::readRecord")
read_end = library.index("void CapsuleLibrary::populateDamagedRecord", read_start)
if "readBestText" in library[read_start:read_end]:
    raise SystemExit("FAIL performance_architecture: metadata hydration reads body text")
require(library, "MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT",
        "capsule locator index is not allocated in PSRAM")
require(library_header, "std::array<DetailCacheEntry, kCapsuleDetailCacheCapacity>",
        "capsule details do not use a bounded cache")
require(index_policy, "kCapsuleLocatorCapacity = 512",
        "capsule locator capacity does not cover 320 fixtures")
require(index_policy, "locatorDamaged",
        "damaged capsule locators are not represented")
require(dashboard, "view.library->at(index, true)",
        "visible rows do not request lazy preview hydration")
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
