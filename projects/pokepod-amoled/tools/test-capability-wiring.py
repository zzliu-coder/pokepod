#!/usr/bin/env python3

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
APP = (ROOT / "firmware/PokePodAmoled/PokePodApp.cpp").read_text()
LINK = (ROOT / "firmware/PokePodAmoled/PokePodLinkService.cpp").read_text()
DASHBOARD = (ROOT / "firmware/PokePodAmoled/Dashboard.cpp").read_text()


def require(source: str, needle: str, message: str) -> None:
    if needle not in source:
        raise SystemExit(f"FAIL capability_wiring: {message}")


require(APP, "bootCapsuleLibraryStarted = board.sdReady() && capsuleLibrary.begin(",
        "CapsuleLibrary::begin result is not captured")
require(APP, "bootTencentWorkerStarted = bootCapsuleLibraryStarted &&",
        "TencentWorker::begin result is not captured")
require(APP, "capabilities.record(DeviceCapability::capsuleLibrary,",
        "capsule-library capability fact is not recorded")
require(APP, "capabilities.record(DeviceCapability::recording,",
        "recorder/capture capability fact is not recorded")
require(APP, "capabilities.record(DeviceCapability::transcription,",
        "ASR worker capability fact is not recorded")
require(APP, "view.library = view.capsuleLibraryReady ? &capsuleLibrary : nullptr;",
        "dashboard can observe an unavailable capsule library")
require(APP, "startupCapabilityPresentation(capabilities)",
        "startup UI does not consume the shared capability registry")
require(APP, "uiActionRequiresCapsuleLibrary(action)",
        "capsule UI actions are not fail-closed")
require(APP, "!capabilities.allows(kTranscriptionCapabilities)",
        "manual ASR retry is not fail-closed")
require(LINK, "requiredCapabilitiesForLinkOperation(operation)",
        "Link operations do not consume the shared capability registry")
require(DASHBOARD, "view.localCapsulesReady",
        "home recording entry ignores the startup capability facts")

library_gate = APP.index("bootTencentWorkerStarted = bootCapsuleLibraryStarted &&")
worker_start = APP.index("tencentWorker.begin(", library_gate)
if not library_gate < worker_start:
    raise SystemExit(
        "FAIL capability_wiring: ASR worker can start without a recovered library")

print("PASS capability_wiring")
