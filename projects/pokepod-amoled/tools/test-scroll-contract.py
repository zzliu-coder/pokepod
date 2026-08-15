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
require(main, "dashboard.updateVerticalScroll(y, now, capsuleLibrary.get())",
        "drag does not follow the live touch position")
require(main, "dashboard.advanceVerticalScroll(now, capsuleLibrary.get())",
        "inertial scrolling is not serviced")
require(main, "ui::kScrollFrameIntervalMs",
        "scroll frame pacing is missing")
require(dashboard, "presentScrollRegion()",
        "scroll frames still present the entire AMOLED frame")
require(dashboard, "composeAndPresentScrollFrame(view)",
        "scroll frames do not use the clipped composition path")
require(dashboard, "drawActiveScrollSurface(view, clipTop, clipBottom)",
        "scroll frames still compose the entire page body")
require(dashboard, "std::memmove(base + reuse.destinationRow",
        "scroll frames do not reuse composed viewport pixels")
require(dashboard, "reuse.exposedRows, ui::kBackground",
        "scroll frames do not limit recomposition to the exposed strip")
require(dashboard, "display_->fillRect(region.x, region.y, region.width",
        "scroll frames still clear the entire indexed canvas")
require(dashboard, "currentStableSignature == lastStableSignature_",
        "scroll-only presentation is not guarded against structural changes")
require(main, "touchVerticalScrolling || dashboard.scrollActive()",
        "active scrolling can still trigger screen timeout")
require(main, "dashboard.advancePageTransition(now)",
        "page transitions are not serviced by the main loop")
require(main, "dashboard.pageTransitionActive()",
        "page transitions do not hold the performance and awake policy")
require(dashboard, "beginPreparedPageTransition(millis())",
        "page changes still replace the full panel immediately")
require(power, "input.uiAnimating", "scrolling does not request performance")
require(renderer, "loadSdCache(codepoint, glyph)",
        "SD glyph cache is not used")
require(renderer, "storeSdCache(codepoint, glyph)",
        "SD glyph cache is not populated")
require(renderer, "sdCacheLookup_[slot]", "SD glyph cache lacks a hot lookup")

if "dashboard.swipeVertical" in main:
    raise SystemExit("FAIL scroll_contract: legacy release-only scrolling remains")

theme = (FIRMWARE / "UiTheme.h").read_text(encoding="utf-8")
require(theme, "kScrollFrameIntervalMs = 20",
        "scroll cadence is below the 50 fps target")

print("PASS scroll_contract")
