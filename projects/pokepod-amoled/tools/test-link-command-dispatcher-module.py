#!/usr/bin/env python3
"""Keep Link v2 JSON routing in one dispatcher implementation unit."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FIRMWARE = ROOT / "firmware" / "PokePodAmoled"
SERVICE = (FIRMWARE / "PokePodLinkService.cpp").read_text(encoding="utf-8")
DISPATCHER = (FIRMWARE / "LinkCommandDispatcher.cpp").read_text(encoding="utf-8")
HEADER = (FIRMWARE / "PokePodLinkService.h").read_text(encoding="utf-8")

METHODS = (
    "processRequest", "handleImmediate", "handleRead", "handleConfigure",
    "beginManifest", "advanceManifest", "advanceManifestScan",
    "advanceManifestFile", "advanceManifestHash", "finishManifestResponse",
    "failManifest", "abortManifest", "cleanupManifestStorage",
)
for method in METHODS:
    signature = f"PokePodLinkService::{method}("
    assert signature in DISPATCHER, f"dispatcher method missing: {method}"
    assert signature not in SERVICE, f"duplicate dispatcher method: {method}"

# Frozen protocol surface and capability gating remain byte/schema compatible.
for operation in (
    "font-write", "stage-write", "command", "status", "fingerprint",
    "record", "stop", "read", "provisioning-start", "link-probe",
    "set-provisioning-password-mode",
):
    assert f'"{operation}"' in DISPATCHER
assert "jsonInt64(root, \"version\") != kLinkVersion" in DISPATCHER
assert "requiredCapabilitiesForLinkOperation(operation)" in DISPATCHER
assert "sendError(requestId, \"malformed Link v2 request\")" in DISPATCHER
assert "sendBusy(requestId)" in DISPATCHER

# Request/admission, deadline and response ownership remain singletons in host.
assert "LinkOperation operation_;" in HEADER
assert "LinkTransferGate *transferGate_" in HEADER
assert "LinkOperation operation_;" not in DISPATCHER
assert "LinkTransferGate transferGate_" not in DISPATCHER
assert "admitLinkOperation(requestId)" in DISPATCHER
assert "transferPermitted()" in DISPATCHER

# Manifest work stays cooperative and never drains in a synchronous loop.
assert "switch (manifestStepper_.phase())" in DISPATCHER
assert "while (manifestStepper_.active())" not in DISPATCHER
assert "StorageAccess::read, 0" in DISPATCHER

print("PASS link_command_dispatcher_module")
