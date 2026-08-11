#!/usr/bin/env python3
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SERVICE = (ROOT / "firmware/PokePodAmoled/BleVoiceService.cpp").read_text(
    encoding="utf-8"
)
HEADER = (ROOT / "firmware/PokePodAmoled/BleVoiceService.h").read_text(
    encoding="utf-8"
)
MAILBOX = (
    ROOT / "firmware/PokePodAmoled/BleVoiceCallbackMailbox.h"
).read_text(encoding="utf-8")
PROTOCOL = (ROOT / "firmware/PokePodAmoled/BleVoiceProtocol.h").read_text(
    encoding="utf-8"
)


def body(signature: str) -> str:
    start = SERVICE.index(signature)
    brace = SERVICE.index("{", start)
    depth = 0
    for index in range(brace, len(SERVICE)):
        if SERVICE[index] == "{":
            depth += 1
        elif SERVICE[index] == "}":
            depth -= 1
            if depth == 0:
                return SERVICE[brace : index + 1]
    raise AssertionError(f"unterminated body: {signature}")


callback_region = SERVICE[
    SERVICE.index("class ServerCallbacks") : SERVICE.index(
        "ServerCallbacks *serverCallbacks"
    )
]
for forbidden in (
    "controller_",
    "router_",
    "audioInFlightFrame_",
    "quality_",
    "connected_",
    "authenticated_",
    "appReady_",
    "mtu_",
    "notifyControl(",
    "setValue(",
):
    assert forbidden not in callback_region, forbidden

for callback_entry in (
    "owner_.handleConnect",
    "owner_.handleDisconnect",
    "owner_.handleMtu",
    "owner_.handleCommand",
    "owner_.handleAuthentication",
    "owner_.handleNotifyStatus",
    "owner_.handleControlNotifyStatus",
    "owner_.handleDeviceInfoRead",
    "owner_.handlePasskey",
):
    assert callback_entry in callback_region, callback_entry

for signature in (
    "void BleVoiceService::handleConnect(",
    "void BleVoiceService::handleDisconnect(",
    "void BleVoiceService::handleMtu(",
    "void BleVoiceService::handleCommand(",
    "void BleVoiceService::handleAuthentication(",
    "void BleVoiceService::handleNotifyStatus(",
    "void BleVoiceService::handleControlNotifyStatus(",
    "void BleVoiceService::handleDeviceInfoRead(",
    "void BleVoiceService::handlePasskey(",
):
    publisher = body(signature)
    assert "publishCallbackEvent(event);" in publisher, signature
    for forbidden in ("controller_", "router_", "quality_", "notifyControl("):
        assert forbidden not in publisher, f"{signature}: {forbidden}"

poll = body("void BleVoiceService::poll(")
assert poll.index("drainCallbackEvents(nowMs);") < poll.index(
    "controller_.poll(nowMs)"
)
assert "processCallbackEvent(event, nowMs);" in body(
    "void BleVoiceService::drainCallbackEvents("
)
assert "controller_.markReady" in body("void BleVoiceService::processCommand(")
assert "controller_.abort" in body("void BleVoiceService::processDisconnect(")
assert "controller_.markSessionEndSent" in body(
    "void BleVoiceService::processControlNotifyStatus("
)

assert "std::atomic<uint32_t> head_{0}" in MAILBOX
assert "std::atomic<uint32_t> tail_{0}" in MAILBOX
assert "class BleVoiceCallbackSecuritySnapshot" in MAILBOX
assert "expected != connectionId" in MAILBOX
assert "callbackSecurity_.observeConnect(connectionId, peerBonded);" in SERVICE
assert "callbackSecurity_.observeDisconnect(connectionId);" in SERVICE
assert "BleVoiceCallbackEvent events_[Capacity]" in MAILBOX
assert "accepting_.store(false" in MAILBOX
assert "overflowed_.store(true" in MAILBOX
assert "static constexpr size_t kCommandBytes = 8" in MAILBOX
assert "static constexpr size_t kCallbackEventCapacity = 16" in HEADER

# BLE Voice v1 UUIDs and wire sizes stay byte-for-byte compatible.
for invariant in (
    "constexpr uint8_t kBleVoiceVersion = 1;",
    "constexpr size_t kBleVoiceHeaderBytes = 15;",
    "constexpr size_t kBleVoicePayloadBytes = 160;",
    '"7A530001-4B50-4F44-9000-504F4B45504F"',
    '"7A530005-4B50-4F44-9000-504F4B45504F"',
):
    assert invariant in PROTOCOL, invariant

print("PASS ble_thread_ownership_contract")
