#!/usr/bin/env python3

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADER = (ROOT / "firmware/PokePodAmoled/PokePodLinkService.h").read_text()
SOURCE = (ROOT / "firmware/PokePodAmoled/PokePodLinkService.cpp").read_text()
DISPATCHER = (ROOT / "firmware/PokePodAmoled/LinkCommandDispatcher.cpp").read_text()
DIAGNOSTICS = (ROOT / "firmware/PokePodAmoled/LinkDiagnostics.cpp").read_text()
FILE_TRANSFER = (ROOT / "firmware/PokePodAmoled/LinkFileTransfer.cpp").read_text()
FILE_TRANSFER_HEADER = (ROOT / "firmware/PokePodAmoled/LinkFileTransfer.h").read_text()
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
                   SOURCE.index("bool PokePodLinkService::linkFileTransferPermitted(")]
assert "operation_.frameDrained" in transmit
assert "LinkOperationFrameRole role" in transmit

abort = SOURCE[SOURCE.index("void PokePodLinkService::linkFileCancelTransmitFrames()"):
                SOURCE.index("fs::FS *PokePodLinkService::linkFileSystem(")]
for reset in (
    "txFrameGeneration_ = 0", "pendingControlGeneration_ = 0",
    "txFrameRole_ = LinkOperationFrameRole::progress",
    "pendingControlRole_ = LinkOperationFrameRole::progress",
):
    assert reset in abort, f"disconnect does not discard queued role: {reset}"

assert "cancelRetainedCoordinator" in SOURCE
assert "!operation_.ownsResource(LinkOperationResource::coordinator)" in SOURCE

assert "StorageReservation reservation_" in FILE_TRANSFER_HEADER
assert "DeferredFileCleanup cleanup_" in FILE_TRANSFER_HEADER
assert "linkFileStorageIoTimeout()" in FILE_TRANSFER
assert "if (!lease)" in FILE_TRANSFER
assert "linkFileSendBusy(requestId)" in FILE_TRANSFER
assert "StorageAccess::read, 250" not in FILE_TRANSFER
assert "LinkOperation operation_" not in FILE_TRANSFER_HEADER
assert "LinkTransferGate" not in FILE_TRANSFER_HEADER

status_start = DISPATCHER.index('if (strcmp(operation, "status") == 0)')
status_end = DISPATCHER.index('} else if (strcmp(operation, "provisioning-start")',
                          status_start)
status_dispatch = DISPATCHER[status_start:status_end]
assert "requestLinkRecordingStop" not in status_dispatch
assert "diagnostics_.statusJson()" in status_dispatch
assert "sendTerminalOrDisconnect(requestId" in status_dispatch
status = DIAGNOSTICS[DIAGNOSTICS.index("String LinkDiagnostics::statusJson() const"):
                     DIAGNOSTICS.index("String LinkDiagnostics::provisioningJson() const")]
assert "kStatusExtraBytes" in status
assert "diagnosticsTruncated" in status
assert "String response =" in status
assert r'\"status\":\"ok\"' in status
assert r'\"version\":2' in status

fallback_start = SOURCE.index("bool PokePodLinkService::sendTerminalOrDisconnect")
fallback_end = SOURCE.index("bool PokePodLinkService::sendEvent", fallback_start)
fallback = SOURCE[fallback_start:fallback_end]
assert "kTerminalQueueError" in fallback
assert "disconnect()" in fallback
assert "operation_.cancel" in fallback

queue_start = SOURCE.index("bool PokePodLinkService::queueFrame")
queue_end = SOURCE.index("void PokePodLinkService::advanceTransmit", queue_start)
queue = SOURCE[queue_start:queue_end]
assert queue.index("txStepper_.beginFrame") < queue.index("operation_.queueFrame")
assert "preserveForFallback" in queue

assert 'sendFrame(LinkFrameType::eventJson' in SOURCE
events = SOURCE[SOURCE.index("bool PokePodLinkService::sendEvent("):
                SOURCE.index("bool PokePodLinkService::sendFrame(")]
assert "LinkOperationFrameRole::progress" in events
assert "binary_ack" in SOURCE

print("PASS link_operation_production_contract")
