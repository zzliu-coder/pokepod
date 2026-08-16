#!/usr/bin/env python3
"""Keep Link v2 transport lifecycle in one implementation unit."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FIRMWARE = ROOT / "firmware/PokePodAmoled"
SERVICE = (FIRMWARE / "PokePodLinkService.cpp").read_text(encoding="utf-8")
TRANSPORT = (FIRMWARE / "LinkTransportSession.cpp").read_text(encoding="utf-8")
HEADER = (FIRMWARE / "PokePodLinkService.h").read_text(encoding="utf-8")
DISPATCHER = (FIRMWARE / "LinkCommandDispatcher.cpp").read_text(encoding="utf-8")

METHODS = (
    "activateConnectionGeneration", "admitLinkOperation", "operationOwns",
    "cancelLinkOperation", "advanceLinkOperationSettlement",
    "recoverStalledLink", "linkProbeJson", "disconnect",
    "requestQuiesce", "quiesced", "pollDeferredCleanup", "poll",
    "consumeByte", "resetFrame", "processFrame", "sendOk", "sendBusy",
    "sendError", "sendTerminalOrDisconnect", "sendEvent", "advanceTransmit",
    "linkFileCancelForDeadline", "linkFileQueueFrame", "transferPermitted",
    "storageIoTimeout",
)
for method in METHODS:
    signature = f"PokePodLinkService::{method}("
    assert signature in TRANSPORT, f"transport method missing: {method}"
    assert signature not in SERVICE, f"duplicate transport method: {method}"

# One service object remains the only owner of lifecycle and transport state.
assert "LinkOperation operation_;" in HEADER
assert "LinkOperation operation_;" not in TRANSPORT
assert "LinkTransferGate *transferGate_" in HEADER
assert "LinkTransferGate transferGate_" not in TRANSPORT
assert "admitLinkOperation(requestId)" in DISPATCHER
assert "operation_.queueFrame(" in TRANSPORT
assert "operation_.frameDrained(" in TRANSPORT
assert "advanceLinkOperationSettlement();" in TRANSPORT
assert "liveness_.noteProgress(millis())" in TRANSPORT
assert "RuntimeDiagnosticStage::linkStallRecovery" in TRANSPORT
assert "usb_->discardHostSessionBuffers();" in TRANSPORT

# Frozen Link v2 framing, CRC and response schema remain unchanged.
assert "{'P', 'P', 'V', '2'}" in TRANSPORT
assert "decodeLinkHeader(" in TRANSPORT
assert "validateLinkPayload(" in TRANSPORT
assert "encodeLinkHeader(" in TRANSPORT
assert "linkCrc32(payload, size)" in TRANSPORT
assert '\\"status\\":\\"ok\\",\\"version\\":2' in TRANSPORT
assert 'cJSON_AddNumberToObject(root, "version", 2)' in TRANSPORT

# Wi-Fi still obeys its external absolute deadline; activity never rearms it.
assert "transferGate_->absoluteDeadline(absoluteDeadlineMs)" in TRANSPORT
assert "linkTransferPermitted(transferGate_, nowMs)" in TRANSPORT
assert "return transferGate_ == nullptr ? 1000U : 0U;" in TRANSPORT
assert "extendDeadline" not in TRANSPORT
assert "restartDeadline" not in TRANSPORT

# Receive and transmit work remain bounded/cooperative.
assert '#include "LinkPollBudget.h"' in HEADER
assert "kLinkPollBudgetBytes = 32768" in TRANSPORT
assert "kLinkPollBudgetUs = 2000" in TRANSPORT
assert "LinkPollBudget budget(" in TRANSPORT
assert "LinkPollPhaseGate gate(" in TRANSPORT
assert "pollDeferredCleanup(gate)" in TRANSPORT
assert "processFrame(&gate)" in TRANSPORT
assert "gate.consumeBytes();" in TRANSPORT
assert "deferredRxByte_ = value;" in TRANSPORT
assert "if (deferredRxByte_ >= 0)" in TRANSPORT
assert "esp_timer_get_time()" in TRANSPORT
assert "kLinkWriteSliceBytes = 512" in TRANSPORT
assert "while (true)" not in TRANSPORT

poll_start = TRANSPORT.index("void PokePodLinkService::poll(uint32_t nowMs)")
poll_end = TRANSPORT.index("void PokePodLinkService::consumeByte(", poll_start)
poll = TRANSPORT[poll_start:poll_end]
assert poll.index("LinkPollBudget budget(") < poll.index(
    "pollDeferredCleanup(gate)"
)
assert poll.index("pollDeferredCleanup(gate)") < poll.index(
    "recoverStalledLink(nowMs)"
)
assert poll.index("recoverStalledLink(nowMs)") < poll.index(
    "recordingSession_.observeAutomaticStop"
)
assert poll.index("recordingSession_.observeAutomaticStop") < poll.index(
    "fileTransfer_.advance()"
)
assert poll.index("fileTransfer_.advance()") < poll.index(
    "advanceManifest(nowMs)"
)

cleanup_start = TRANSPORT.index("bool PokePodLinkService::pollDeferredCleanup(")
cleanup_end = TRANSPORT.index("void PokePodLinkService::pollDeferredCleanup()")
cleanup = TRANSPORT[cleanup_start:cleanup_end]
for phase in (
    "advanceCommandLoad", "advanceBatchCommand", "advanceTransactionRunner",
    "advanceBatchStartupRecovery", "recordingSession_.poll",
    "fileTransfer_.pollCleanup", "cleanupManifestStorage",
    "advanceLinkOperationSettlement",
):
    assert phase in cleanup
assert cleanup.count("gate.run(") >= 13

frame_start = TRANSPORT.index("void PokePodLinkService::processFrame(")
frame_end = TRANSPORT.index("bool PokePodLinkService::sendOk(", frame_start)
frame = TRANSPORT[frame_start:frame_end]
validation = frame.index("validateLinkPayload(")
dispatch = frame.index("processRequest(")
assert frame.index("gate->checkpoint()") < validation
assert frame.index("gate->checkpoint()", validation) < dispatch
assert frame.rindex("gate->checkpoint()") > dispatch

receive_loop_start = poll.index("while (gate.checkpoint()")
receive_loop_end = poll.index("if (!gate.checkpoint()) return;",
                              receive_loop_start)
receive_loop = poll[receive_loop_start:receive_loop_end]
assert receive_loop.index("gate.checkpoint()") < receive_loop.index(
    "stream_->available()"
)
assert receive_loop.index("stream_->read()") < receive_loop.index(
    "if (!gate.checkpoint())"
)

print("PASS link_transport_session_module")
