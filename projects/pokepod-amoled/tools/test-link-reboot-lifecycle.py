#!/usr/bin/env python3
"""Lock accepted Link reboot to the device lifecycle, not a transport epoch."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FW = ROOT / "firmware" / "PokePodAmoled"
transport = (FW / "LinkTransportSession.cpp").read_text(encoding="utf-8")
header = (FW / "PokePodLinkService.h").read_text(encoding="utf-8")
dispatcher = (FW / "LinkCommandDispatcher.cpp").read_text(encoding="utf-8")
wireless_h = (FW / "WirelessSyncService.h").read_text(encoding="utf-8")
wireless_cpp = (FW / "WirelessSyncService.cpp").read_text(encoding="utf-8")
coordinator = (FW / "DeviceRebootCoordinator.h").read_text(encoding="utf-8")
app = (FW / "PokePodApp.cpp").read_text(encoding="utf-8")
worker = (FW / "TencentWorker.cpp").read_text(encoding="utf-8")

disconnect = transport[transport.index("void PokePodLinkService::disconnect()"):]
disconnect = disconnect[:disconnect.index("void PokePodLinkService::requestQuiesce")]
assert "rebootAtMs_" not in disconnect
assert "RebootQuiescePhase" not in disconnect
assert "rebootAtMs_" not in header
assert "RebootQuiescePhase" not in header
assert "pollReboot" not in header
assert "pollReboot" not in transport

assert "DeviceRebootCoordinator *rebootCoordinator_" in header
assert "sendOk(requestId)" in dispatcher
assert "rebootCoordinator_->request(millis(), transport_)" in dispatcher
assert dispatcher.index("sendOk(requestId)") < dispatcher.index(
    "rebootCoordinator_->request(millis(), transport_)")
assert "DeviceRebootCoordinator &rebootCoordinator" in wireless_h
assert "&rebootCoordinator" in wireless_cpp
assert "DeviceRebootCoordinator deviceReboot;" in app
assert "&capabilities, &deviceReboot" in app
assert "linkCoordinator, deviceReboot, usb.log()" in app
assert "bool request(uint32_t nowMs, LinkTransport transport)" in coordinator
assert "void acknowledgeRestart()" in coordinator
assert "transport disconnect" in coordinator

assert "deviceReboot.pending()" in app
assert "deviceReboot.beginServiceQuiesce()" in app
assert "deviceReboot.acknowledgeRestart()" in app
assert "deviceReboot.defer(now)" in app
loop = app[app.index("void loop()"):] 
assert loop.index("if (deviceReboot.pending())") < loop.index(
    "pollDeferredServiceCleanup()")

begin = worker[worker.index("TencentQuiesceStatus TencentWorker::beginQuiesce"):
               worker.index("void TencentWorker::taskEntry")]
assert "finishAttempt" not in begin
assert "abandonResultForReboot" in worker
assert "runtime_.abandonForReboot" in worker

print("PASS link_reboot_lifecycle")
