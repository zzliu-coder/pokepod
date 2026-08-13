#!/usr/bin/env python3
"""Prove the capsule command implementation has one dedicated module."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FIRMWARE = ROOT / "firmware" / "PokePodAmoled"
SERVICE = (FIRMWARE / "PokePodLinkService.cpp").read_text(encoding="utf-8")
COMMANDS = (FIRMWARE / "LinkCapsuleCommands.cpp").read_text(encoding="utf-8")
HEADER = (FIRMWARE / "PokePodLinkService.h").read_text(encoding="utf-8")

METHODS = (
    "handleCommandFile", "beginCommandLoad", "advanceCommandLoad",
    "dispatchLoadedCommand", "tryStartTextCommand", "tryStartBatchCommand",
    "advanceBatchCommand", "advanceBatchPending", "finishBatchCommand",
    "startBatchStartupRecovery", "advanceBatchStartupRecovery",
    "finishCommandStorageCleanup", "stepDeferredTreeCleanup",
    "stepDeferredFileCleanup",
)
for method in METHODS:
    signature = f"PokePodLinkService::{method}("
    assert signature in COMMANDS, f"missing command module method: {method}"
    assert signature not in SERVICE, f"duplicate command implementation: {method}"

# Single-owner invariants remain in the existing PokePodLinkService object.
assert "LinkOperation operation_;" in HEADER
assert "CapsuleTransactionRunner transactionRunner_;" in HEADER
assert "StorageOwner storageOwner() const;" in HEADER
assert "LinkOperation operation_;" not in COMMANDS
assert "class LinkOperation" not in COMMANDS

# Durable command results still precede terminal success and cleanup.
finish = COMMANDS[COMMANDS.index("void PokePodLinkService::finishBatchCommand("):]
assert "commandStorageReservation_.release()" in finish
assert "operation_.releaseResource(LinkOperationResource::transaction)" in finish
assert "operation_.releaseResource(LinkOperationResource::storageReservation)" in finish
assert "sendOk(requestId, \"\\\"accepted\\\":true\")" in finish
assert finish.index("commandStorageReservation_.release()") < finish.index(
    "sendOk(requestId, \"\\\"accepted\\\":true\")"
)

# Wi-Fi still uses the host's absolute deadline and zero-wait storage timeout.
loader = COMMANDS[
    COMMANDS.index("void PokePodLinkService::advanceCommandLoad("):
    COMMANDS.index("void PokePodLinkService::finishCommandLoad(")
]
assert "transferPermitted()" in loader
assert "StorageAccess::read, 0" in loader
assert "storageIoTimeout()" in COMMANDS

print("PASS link_capsule_commands_module")
