#!/usr/bin/env python3
"""Lock accepted Link reboot to the device lifecycle, not a transport epoch."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FW = ROOT / "firmware" / "PokePodAmoled"
transport = (FW / "LinkTransportSession.cpp").read_text(encoding="utf-8")
header = (FW / "PokePodLinkService.h").read_text(encoding="utf-8")
app = (FW / "PokePodApp.cpp").read_text(encoding="utf-8")
worker = (FW / "TencentWorker.cpp").read_text(encoding="utf-8")

disconnect = transport[transport.index("void PokePodLinkService::disconnect()"):]
disconnect = disconnect[:disconnect.index("bool PokePodLinkService::pollReboot")]
assert "rebootAtMs_ = 0" not in disconnect
assert "rebootQuiescePhase_ = RebootQuiescePhase::idle" not in disconnect
assert "bool PokePodLinkService::pollReboot" in transport
assert "if (stream_ != nullptr) stream_->flush();" in transport
assert "bool rebootPending() const" in header
assert "linkService.pollReboot(now)" in app
assert "linkService.acknowledgeReboot()" in app
assert "linkService.deferReboot(now)" in app
loop = app[app.index("void loop()"):] 
assert loop.index("if (linkService.rebootPending())") < loop.index(
    "pollDeferredServiceCleanup()")

begin = worker[worker.index("TencentQuiesceStatus TencentWorker::beginQuiesce"):
               worker.index("void TencentWorker::taskEntry")]
assert "finishAttempt" not in begin
assert "abandonResultForReboot" in worker
assert "runtime_.abandonForReboot" in worker

print("PASS link_reboot_lifecycle")
