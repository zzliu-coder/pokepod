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
scan_start = library.index("bool CapsuleLibrary::processDirectorySlice")
scan_end = library.index("bool CapsuleLibrary::openPendingMetadata", scan_start)
if "readBestText" in library[scan_start:scan_end]:
    raise SystemExit("FAIL performance_architecture: scan eagerly reads preview text")
read_start = library.index("bool CapsuleLibrary::readRecord")
read_end = library.index("void CapsuleLibrary::populateDamagedRecord", read_start)
if "readBestText" in library[read_start:read_end]:
    raise SystemExit("FAIL performance_architecture: metadata hydration reads body text")
require(library, "MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT",
        "capsule locator index is not allocated in PSRAM")
require(library, "std::swap(locators_, scanLocators_)",
        "full scans do not atomically publish a staged PSRAM index")
require(library, "scanOwner_, StorageAccess::read, 0",
        "scan slices can block waiting for the physical SD mutex")
require(library_header, "CapsuleScanState stepScan()",
        "capsule scan is not exposed as a bounded production service")
require(library_header, "std::array<DetailCacheEntry, kCapsuleDetailCacheCapacity>",
        "capsule details do not use a bounded cache")
require(index_policy, "kCapsuleLocatorCapacity = 512",
        "capsule locator capacity does not cover 320 fixtures")
require(index_policy, "sizeof(CapsuleLocator) <= 160",
        "capsule locator lacks a per-record PSRAM budget gate")
require(index_policy,
        "sizeof(CapsuleLocator) * kCapsuleLocatorCapacity <= 80 * 1024",
        "capsule locator index lacks a total PSRAM budget gate")
require(index_policy,
        "sizeof(CapsuleLocator) * kCapsuleLocatorCapacity * 2 <=",
        "double-buffered capsule indexes lack a PSRAM budget gate")
for eager_detail in (
    "char directory[", "char folder[", "char title[", "char updatedAt[",
    "char errorStage[", "char error[", "char audioFile[",
    "char audioFormat[",
):
    if eager_detail in index_policy:
        raise SystemExit(
            "FAIL performance_architecture: capsule locator still embeds "
            f"lazy detail {eager_detail}"
        )
require(library, "hydrateLocator(locators_[locatorIndex]",
        "detail cache misses do not hydrate capsule metadata on demand")
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
require(app, "AudioCaptureRuntime captureRuntime;",
        "realtime capture runtime is not wired into the product")
require(app, "captureRuntime.start(audio, captureSessionId, usb.log())",
        "local recording bypasses the capture task")
require(app, "captureRuntime.start(audio, sessionId, usb.log())",
        "BLE voice bypasses the capture task")
require(app, "recorder.appendMono16(frame.samples",
        "capture frames are not delivered to the recorder")
require(app, "bleVoice.appendMono16(frame.samples",
        "capture frames are not delivered to BLE voice")
if "audio.read(audioBuffer" in app:
    raise SystemExit("FAIL performance_architecture: main loop still reads I2S")
require(renderer, "sdCacheLookup_[slot]",
        "SD glyph cache lacks direct hot lookup")

print("PASS performance_architecture")
