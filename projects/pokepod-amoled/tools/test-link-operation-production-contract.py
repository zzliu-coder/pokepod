#!/usr/bin/env python3

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADER = (ROOT / "firmware/PokePodAmoled/PokePodLinkService.h").read_text()
SOURCE = (ROOT / "firmware/PokePodAmoled/PokePodLinkService.cpp").read_text()
GATE = (ROOT / "firmware/PokePodAmoled/LinkTransferGate.h").read_text()
MATRIX = (ROOT / "firmware/tests/link_operation_migration.md").read_text()

assert '#include "LinkOperation.h"' in HEADER
for symbol in (
    "LinkOperation operation_", "connectionGeneration_",
    "nextConnectionGeneration_", "admitLinkOperation",
    "cancelLinkOperation", "advanceLinkOperationSettlement",
):
    assert symbol in HEADER or symbol in SOURCE, f"missing adapter fact: {symbol}"

for operation in (
    "Immediate read/configure/reboot",
    "Incoming staged file or system font",
    "Incoming command: batch/text/simple",
    "Outgoing file/read/result", "Recursive/shallow manifest",
    "Record start", "Record stop", "Maintenance begin/end",
):
    assert operation in MATRIX, f"missing migration row: {operation}"

for column in (
    "Admit point", "Progress / data / terminal frames",
    "Durable terminal fact", "Resources acquired / released",
    "Cancel and rollback", "Completed eligibility", "Maintenance retain",
):
    assert column in MATRIX, f"missing migration column: {column}"

assert "absoluteDeadline(uint32_t &deadlineMs) const" in GATE
absolute = GATE[GATE.index("bool absoluteDeadline(uint32_t &deadlineMs) const override"):
                GATE.index("bool active() const")]
assert "deadlineMs = deadlineMs_" in absolute
assert ".arm(" not in absolute

for legacy in (
    "requestLeaseHeld_", "requestLeaseOwnerRequestId_",
    "releaseRequestLeaseWhenTxDrained_", "rememberCompleted(",
    "releaseRequestLease(",
):
    assert legacy not in HEADER
    assert legacy not in SOURCE

queue = SOURCE[SOURCE.index("bool PokePodLinkService::queueFrame("):
               SOURCE.index("void PokePodLinkService::advanceTransmit(")]
assert "operation_.queueFrame(" in queue
assert "completed_.complete" not in queue
assert "coordinator_->release" not in queue
assert "failOwnedQueue" in queue
assert "operation_.discardQueuedFrames" in queue

settle = SOURCE[SOURCE.index(
    "void PokePodLinkService::advanceLinkOperationSettlement()"):
    SOURCE.index("void PokePodLinkService::disconnect()")]
assert SOURCE.count("completed_.complete") == 1
assert "completed_.complete" in settle
assert "completed_.contains(requestId) || completed_.complete(requestId)" in settle
assert "operation_.connectionGeneration() == connectionGeneration_" in settle
assert SOURCE.count("coordinator_->release") == 2  # admission rollback + settlement
assert "coordinator_->release" in settle
assert "coordinator_->owner() == transport_" in settle
assert "coordinator_->owner() == LinkTransport::none" in settle

disconnect = SOURCE[SOURCE.index("void PokePodLinkService::disconnect()"):
                    SOURCE.index("void PokePodLinkService::requestQuiesce()")]
assert "cancelLinkOperation" in disconnect
assert "advanceLinkOperationSettlement" in disconnect
assert "connectionGeneration_ = 0" in disconnect
assert "completed_.clear()" not in disconnect

generation = SOURCE[SOURCE.index(
    "uint32_t PokePodLinkService::activateConnectionGeneration()"):
    SOURCE.index("LinkOperationAdmission PokePodLinkService::admitLinkOperation")]
assert "completed_.clear()" in generation

transmit = SOURCE[SOURCE.index("void PokePodLinkService::advanceTransmit("):
                   SOURCE.index("void PokePodLinkService::queueNextFileChunk(")]
assert "operation_.frameDrained" in transmit
assert "LinkOperationFrameRole role" in transmit

abort = SOURCE[SOURCE.index("void PokePodLinkService::abortOutgoing()"):
                SOURCE.index("void PokePodLinkService::onFrameSent(")]
for reset in (
    "txFrameGeneration_ = 0", "pendingControlGeneration_ = 0",
    "txFrameRole_ = LinkOperationFrameRole::progress",
    "pendingControlRole_ = LinkOperationFrameRole::progress",
):
    assert reset in abort, f"disconnect does not discard queued role: {reset}"

assert "cancelRetainedCoordinator" in SOURCE
assert "!operation_.ownsResource(LinkOperationResource::coordinator)" in SOURCE

send_file = SOURCE[SOURCE.index("bool PokePodLinkService::sendFile("):
                   SOURCE.index("bool PokePodLinkService::sendFrame(")]
assert "storageIoTimeout()" in send_file
assert "if (!lease)" in send_file
assert "sendBusy(requestId)" in send_file
assert "StorageAccess::read, 250" not in send_file

status_start = SOURCE.index('if (strcmp(operation, "status") == 0)')
status_end = SOURCE.index('} else if (strcmp(operation, "provisioning-start")',
                          status_start)
status = SOURCE[status_start:status_end]
assert "requestLinkRecordingStop" not in status

assert 'sendFrame(LinkFrameType::eventJson' in SOURCE
events = SOURCE[SOURCE.index("bool PokePodLinkService::sendEvent("):
                SOURCE.index("bool PokePodLinkService::sendFile(")]
assert "LinkOperationFrameRole::progress" in events
assert "binary_ack" in SOURCE

print("PASS link_operation_production_contract")
