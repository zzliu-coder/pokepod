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

# Generic state entry is intentionally absent. Special phases are reachable
# only through methods that update their companion facts.
assert "bool enter(" not in CORE
assert "bool advance(" in CORE
advance = CORE[CORE.index("bool advance("):CORE.index("bool observeDeadline(")]
assert "ordinaryWorkState(next)" in advance

frames = CORE[CORE.index("bool queueFrame("):CORE.index("void ownResource(")]
assert "connectionGeneration != connectionGeneration_" in frames
assert "terminalFrameQueued_" in frames
assert "progressFramesQueued_" in frames
assert "dataFramesQueued_" in frames
assert "discardQueuedFrames" in frames

settlement = CORE[CORE.index("LinkOperationSettlement settlement()"):
                  CORE.index("bool finishRelease()")]
assert "allFramesDrained()" in settlement
assert "operationalResourcesDrained()" in settlement
assert "completionAckPending_" in settlement
assert "requestReleaseAckPending_" in settlement
assert "coordinatorReleaseAckPending_" in settlement
assert "acknowledgeCompletion" in settlement
assert "acknowledgeRelease" in settlement

# Release is proposed, acknowledged, then finalized. beginRelease must not
# clear external resource bits optimistically.
begin_release = settlement[settlement.index("bool beginRelease()"):
                           settlement.index("bool acknowledgeCompletion(")]
assert "releaseResource(" not in begin_release

assert "connectionGeneration == 0" in CORE
assert "cancelRetainedCoordinator" in CORE
assert "bool active() const { return state_ != LinkOperationState::idle; }" in CORE
assert "static_cast<uint32_t>(progressFramesQueued_)" in CORE
assert "responseDrained_ && allFramesDrained()" in CORE
assert "static_cast<int32_t>(nowMs - absoluteDeadlineMs_)" in CORE
print("PASS link_operation_contract")
