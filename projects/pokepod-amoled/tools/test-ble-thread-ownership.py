#!/usr/bin/env python3
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SERVICE = (ROOT / "firmware/PokePodAmoled/BleVoiceService.cpp").read_text(
    encoding="utf-8"
)
MAIN = (ROOT / "firmware/PokePodAmoled/PokePodApp.cpp").read_text(
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
OVERFLOW_POLICY = (
    ROOT / "firmware/PokePodAmoled/BleCallbackOverflowPolicy.h"
).read_text(encoding="utf-8")


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
    "void BleVoiceService::handleDeviceInfoRead(",
    "void BleVoiceService::handlePasskey(",
):
    publisher = body(signature)
    assert "publishCallbackEvent(event);" in publisher, signature
    for forbidden in ("controller_", "router_", "quality_", "notifyControl("):
        assert forbidden not in publisher, f"{signature}: {forbidden}"

for signature, kind in (
    ("void BleVoiceService::handleNotifyStatus(", "BleVoiceNotifyKind::audio"),
    (
        "void BleVoiceService::handleControlNotifyStatus(",
        "BleVoiceNotifyKind::control",
    ),
):
    publisher = body(signature)
    assert "notifyCallbackBinding_.capture" in publisher, signature
    assert kind in publisher, signature
    assert "notifyStatusEvents_.publish(event);" in publisher, signature
    for forbidden in ("controller_", "router_", "quality_", "notifyControl("):
        assert forbidden not in publisher, f"{signature}: {forbidden}"

poll = body("void BleVoiceService::poll(")
assert poll.index("drainCallbackEvents(nowMs);") < poll.index(
    "controller_.poll(nowMs)"
)
assert "audioNotifyIdentity_ = {};" in poll
assert "processCallbackEvent(event, nowMs);" in body(
    "void BleVoiceService::drainCallbackEvents("
)
assert "controller_.markReady" in body("void BleVoiceService::processCommand(")
assert "controller_.abort" in body("void BleVoiceService::processDisconnect(")
assert "controller_.markSessionEndSent" in body(
    "void BleVoiceService::processControlNotifyStatus("
)
for signature in (
    "void BleVoiceService::processNotifyStatus(",
    "void BleVoiceService::processControlNotifyStatus(",
):
    resolver = body(signature)
    assert "event.notifyIdentity.matches" in resolver, signature
    assert "connectionGeneration_" in resolver, signature
    assert "sessionGeneration_" in resolver, signature

clear_control = body("void BleVoiceService::clearControlNotify(")
assert "notifyCallbackBinding_.invalidate();" in clear_control
assert "controlNotifyIdentity_ = {};" in clear_control
reset_audio = body("void BleVoiceService::resetAudioNotify(")
assert "notifyCallbackBinding_.invalidate();" in reset_audio
assert "audioNotifyIdentity_ = {};" in reset_audio

assert "std::atomic<uint32_t> head_{0}" in MAILBOX
assert "std::atomic<uint32_t> tail_{0}" in MAILBOX
assert "class BleVoiceCallbackSecuritySnapshot" in MAILBOX
assert "class BleVoiceNotifyCallbackBinding" in MAILBOX
assert "struct BleVoiceNotifyIdentity" in MAILBOX
assert "class BleVoicePhysicalDisconnectLatch" in MAILBOX
assert "expected != connectionId" in MAILBOX
connect_callback = body("void BleVoiceService::handleConnect(")
assert "callbackSecurity_.observeConnect" in connect_callback
assert "&event.connectionGeneration" in connect_callback
disconnect_callback = body("void BleVoiceService::handleDisconnect(")
assert "callbackSecurity_.observeDisconnect" in disconnect_callback
assert "&event.connectionGeneration" in disconnect_callback
assert "BleVoiceCallbackEvent events_[Capacity]" in MAILBOX
assert "accepting_.store(false" in MAILBOX
assert "overflowed_.store(true" in MAILBOX
assert "ownerTask != callbackTask" in MAILBOX
assert "attemptToken == other.attemptToken" in MAILBOX
assert "static constexpr size_t kCommandBytes = 8" in MAILBOX
assert "static constexpr size_t kCallbackEventCapacity = 16" in HEADER
assert "static constexpr size_t kNotifyStatusEventCapacity = 4" in HEADER
assert "bool empty() const" in MAILBOX

overflow = body("void BleVoiceService::drainCallbackEvents(")
assert "resetAfterOverflow" not in overflow
overflow_advance = body("void BleVoiceService::advanceCallbackOverflow(")
assert "finishCallbackOverflowIfDisconnected(nowMs)" in overflow_advance
assert "callbackOverflow_.poll(nowMs)" in overflow_advance
assert "server_->disconnect(callbackOverflow_.epoch().connectionId)" in \
    overflow_advance
assert "ble_voice_callback_overflow_hard_failed" in overflow_advance
overflow_finish = body(
    "bool BleVoiceService::finishCallbackOverflowIfDisconnected("
)
assert "callbackOverflow_.confirm(physicalDisconnect)" in overflow_finish
assert overflow_finish.index("callbackOverflow_.confirm") < overflow_finish.index(
    "resetAfterOverflow"
)
assert "physicalDisconnects_.observe" in disconnect_callback
assert "kDisconnectRetryMs = 250" in OVERFLOW_POLICY
assert "kCleanupDeadlineMs = 5000" in OVERFLOW_POLICY
assert "BleCallbackOverflowPhase::hardFailed" in OVERFLOW_POLICY
assert "requiresProcessRecovery()" in OVERFLOW_POLICY
assert "recoverInvalidEpoch()" in OVERFLOW_POLICY
assert "reached(nowMs, deadlineMs_)" in OVERFLOW_POLICY
assert "reached(nowMs, nextRetryAtMs_)" in OVERFLOW_POLICY
assert "physicalDisconnect.matches(epoch_)" in OVERFLOW_POLICY
assert "bool quiescedForSleep() const" in HEADER
assert "return bleVoiceQuiescedForSleep(sleepQuiescenceFacts())" in HEADER
assert "facts.overflowCleanupActive = callbackOverflow_.active()" in HEADER
assert "facts.physicalConnectionPending = physicalConnectionPending()" in HEADER
assert "facts.callbackMailboxEmpty = callbackEvents_.empty()" in HEADER
assert "facts.notifyMailboxEmpty = notifyStatusEvents_.empty()" in HEADER
pause = body("bool BleVoiceService::pauseForIdleSleep()")
assert "physicalConnectionPending()" in pause
assert "return quiescedForSleep();" in pause
assert "callbackOverflowRecoveryRequired()" in HEADER
assert "claimCallbackOverflowRecoveryRestart()" in HEADER
assert "ESP.restart()" in MAIN

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
