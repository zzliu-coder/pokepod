#!/usr/bin/env python3

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
FIRMWARE = ROOT / "firmware" / "PokePodAmoled"


def require(source: str, needle: str, label: str) -> None:
    if needle not in source:
        raise SystemExit(f"FAIL scroll_contract: {label}")


dashboard = (FIRMWARE / "Dashboard.cpp").read_text(encoding="utf-8")
main = (FIRMWARE / "PokePodApp.cpp").read_text(encoding="utf-8")
renderer = (FIRMWARE / "ChineseRenderer.cpp").read_text(encoding="utf-8")
power = (FIRMWARE / "PowerPolicy.h").read_text(encoding="utf-8")

for surface in ("capsuleScroll_", "detailScroll_", "provisioningLogScroll_"):
    require(dashboard, f"return &{surface};", f"missing {surface} routing")

require(dashboard, "wrappedLineCount(detailBodyCache_",
        "detail extent is not measured")
require(dashboard, "contentHeight - viewportHeight",
        "scroll bounds are not clamped to content")
require(dashboard, "capsuleScroll_.positionPx() + y -",
        "capsule hit testing ignores pixel scroll")
require(dashboard, "libraryRevision != lastLibraryRevision_",
        "scroll frames still reconcile the full capsule library")
require(main, "dashboard.updateVerticalScroll(y, now, capsuleLibrary)",
        "drag does not follow the live touch position")
require(main, "dashboard.advanceVerticalScroll(now, capsuleLibrary)",
        "inertial scrolling is not serviced")
require(main, "ui::kScrollFrameIntervalMs",
        "scroll frame pacing is missing")
require(dashboard, "presentScrollRegion()",
        "scroll frames still present the entire AMOLED frame")
require(dashboard, "currentStableSignature == lastStableSignature_",
        "scroll-only presentation is not guarded against structural changes")
require(main, "touchVerticalScrolling || dashboard.scrollActive()",
        "active scrolling can still trigger screen timeout")
require(power, "input.uiAnimating", "scrolling does not request performance")
require(renderer, "loadSdCache(codepoint, glyph)",
        "SD glyph cache is not used")
require(renderer, "storeSdCache(codepoint, glyph)",
        "SD glyph cache is not populated")

if "dashboard.swipeVertical" in main:
    raise SystemExit("FAIL scroll_contract: legacy release-only scrolling remains")

print("PASS scroll_contract")
