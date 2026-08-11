#!/usr/bin/env python3

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CORE = (ROOT / "firmware/PokePodAmoled/LinkOperation.h").read_text()

assert "#include <Arduino.h>" not in CORE
assert "class LinkOperation" in CORE
for state in (
    "idle", "accepted", "receiving", "processing", "durableCommit",
    "cleanup", "terminalResponse", "responseDrained", "cancelled",
    "rollback", "blocked", "release",
):
    assert state in CORE, f"missing lifecycle state: {state}"

for resource in (
    "request", "coordinator", "storageReservation", "file", "router",
    "transaction",
):
    assert resource in CORE, f"missing owned resource: {resource}"

progress = CORE[CORE.index("bool queueFrame("):
                CORE.index("bool frameDrained(")]
assert "role != LinkOperationFrameRole::terminal" in progress
assert "completionEligible_ = completionEligible" in progress

settlement = CORE[CORE.index("LinkOperationSettlement settlement()"):
                  CORE.index("bool beginRelease()")]
assert "operationalResourcesDrained()" in settlement
assert "terminal && completionEligible_" in settlement
assert "retainCoordinator_" in settlement

assert "connectionGeneration != connectionGeneration_" in CORE
assert "static_cast<int32_t>(nowMs - absoluteDeadlineMs_)" in CORE
print("PASS link_operation_contract")
