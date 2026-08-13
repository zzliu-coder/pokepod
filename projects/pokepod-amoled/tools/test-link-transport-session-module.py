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
    "cancelLinkOperation", "advanceLinkOperationSettlement", "disconnect",
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
assert "kLinkReadBudgetBytes = 32768" in TRANSPORT
assert "kLinkReadBudgetUs = 2000" in TRANSPORT
assert "LinkPollBudget budget(" in TRANSPORT
assert "budget.permits(budgetNowUs)" in TRANSPORT
assert "budget.consume();" in TRANSPORT
assert "esp_timer_get_time()" in TRANSPORT
assert "kLinkWriteSliceBytes = 512" in TRANSPORT
assert "while (true)" not in TRANSPORT

print("PASS link_transport_session_module")
