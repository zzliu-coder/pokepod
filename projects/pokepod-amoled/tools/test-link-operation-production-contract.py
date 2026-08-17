#!/usr/bin/env python3

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADER = (ROOT / "firmware/PokePodAmoled/PokePodLinkService.h").read_text()
SOURCE = (ROOT / "firmware/PokePodAmoled/PokePodLinkService.cpp").read_text()
TRANSPORT = (ROOT / "firmware/PokePodAmoled/LinkTransportSession.cpp").read_text()
IMPLEMENTATION = SOURCE + TRANSPORT
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
    assert symbol in HEADER or symbol in IMPLEMENTATION, f"missing adapter fact: {symbol}"

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
    assert legacy not in IMPLEMENTATION

queue = TRANSPORT[TRANSPORT.index("bool PokePodLinkService::queueFrame("):
                  TRANSPORT.index("void PokePodLinkService::advanceTransmit(")]
assert "operation_.queueFrame(" in queue
assert "completed_.complete" not in queue
assert "coordinator_->release" not in queue
assert "failOwnedQueue" in queue
assert "operation_.discardQueuedFrames" in queue

settle = TRANSPORT[TRANSPORT.index(
    "void PokePodLinkService::advanceLinkOperationSettlement()"):
    TRANSPORT.index("void PokePodLinkService::disconnect()")]
assert TRANSPORT.count("completed_.complete") == 1
assert "completed_.complete" in settle
assert "completed_.contains(requestId) || completed_.complete(requestId)" in settle
assert "operation_.connectionGeneration() == connectionGeneration_" in settle
assert TRANSPORT.count("coordinator_->release") == 2  # admission rollback + settlement
assert "coordinator_->release" in settle
assert "coordinator_->owner() == transport_" in settle
assert "coordinator_->owner() == LinkTransport::none" in settle

disconnect = TRANSPORT[TRANSPORT.index("void PokePodLinkService::disconnect()"):
                       TRANSPORT.index("void PokePodLinkService::requestQuiesce()")]
assert "cancelLinkOperation" in disconnect
assert "advanceLinkOperationSettlement" in disconnect
assert "connectionGeneration_ = 0" in disconnect
assert "completed_.clear()" not in disconnect

generation = TRANSPORT[TRANSPORT.index(
    "uint32_t PokePodLinkService::activateConnectionGeneration()"):
    TRANSPORT.index("LinkOperationAdmission PokePodLinkService::admitLinkOperation")]
assert "completed_.clear()" in generation

transmit = TRANSPORT[TRANSPORT.index("void PokePodLinkService::advanceTransmit("):
                      TRANSPORT.index("bool PokePodLinkService::linkFileTransferPermitted(")]
assert "operation_.frameDrained" in transmit
assert "LinkOperationFrameRole role" in transmit

abort = TRANSPORT[TRANSPORT.index("void PokePodLinkService::linkFileCancelTransmitFrames()"):
                   TRANSPORT.index("fs::FS *PokePodLinkService::linkFileSystem(")]
for reset in (
    "txFrameGeneration_ = 0", "pendingControlGeneration_ = 0",
    "txFrameRole_ = LinkOperationFrameRole::progress",
    "pendingControlRole_ = LinkOperationFrameRole::progress",
):
    assert reset in abort, f"disconnect does not discard queued role: {reset}"

assert "cancelRetainedCoordinator" in TRANSPORT
assert "!operation_.ownsResource(LinkOperationResource::coordinator)" in TRANSPORT

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
status = DIAGNOSTICS[DIAGNOSTICS.index("const String &LinkDiagnostics::statusJson() const"):
                     DIAGNOSTICS.index("String LinkDiagnostics::provisioningJson() const")]
assert "diagnosticsTruncated" in status
assert "String &extra = statusBuffer_" in status
assert "String extra" not in status
assert "statusBuffer_.reserve(kLinkMaxControlBytes)" in DIAGNOSTICS
assert "jsonEscaped" not in DIAGNOSTICS
assert 'extra = kStatusPrefix' in status
assert "extra += '}'" in status
assert r'\"status\":\"ok\"' in DIAGNOSTICS
assert r'\"version\":2' in DIAGNOSTICS

fallback_start = TRANSPORT.index("bool PokePodLinkService::sendTerminalOrDisconnect")
fallback_end = TRANSPORT.index("bool PokePodLinkService::sendEvent", fallback_start)
fallback = TRANSPORT[fallback_start:fallback_end]
assert "kTerminalQueueError" in fallback
assert "disconnect()" in fallback
assert "operation_.cancel" in fallback

queue_start = TRANSPORT.index("bool PokePodLinkService::queueFrame")
queue_end = TRANSPORT.index("void PokePodLinkService::advanceTransmit", queue_start)
queue = TRANSPORT[queue_start:queue_end]
assert queue.index("txStepper_.beginFrame") < queue.index("operation_.queueFrame")
assert "preserveForFallback" in queue

assert 'sendFrame(LinkFrameType::eventJson' in TRANSPORT
events = TRANSPORT[TRANSPORT.index("bool PokePodLinkService::sendEvent("):
                   TRANSPORT.index("bool PokePodLinkService::sendFrame(")]
assert "LinkOperationFrameRole::progress" in events
assert "binary_ack" in IMPLEMENTATION

print("PASS link_operation_production_contract")
