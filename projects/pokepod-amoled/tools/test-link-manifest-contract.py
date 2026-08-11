#!/usr/bin/env python3

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SERVICE = (ROOT / "firmware/PokePodAmoled/PokePodLinkService.cpp").read_text()
HEADER = (ROOT / "firmware/PokePodAmoled/LinkManifestStepper.h").read_text()


def body(signature: str, following: str) -> str:
    start = SERVICE.index(signature)
    end = SERVICE.index(following, start)
    return SERVICE[start:end]


poll = body("void PokePodLinkService::poll", "void PokePodLinkService::consumeByte")
advance = body(
    "void PokePodLinkService::advanceManifest",
    "void PokePodLinkService::advanceManifestScan",
)
hash_step = body(
    "void PokePodLinkService::advanceManifestHash",
    "void PokePodLinkService::finishManifestResponse",
)
response = body(
    "void PokePodLinkService::finishManifestResponse",
    "void PokePodLinkService::failManifest",
)
pending_response = body(
    "void PokePodLinkService::finishPendingManifestResponse",
    "void PokePodLinkService::finishPendingManifestFailure",
)
cleanup = body(
    "bool PokePodLinkService::cleanupManifestStorage",
    "void PokePodLinkService::finishPendingManifestResponse",
)
disconnect = body(
    "void PokePodLinkService::disconnect",
    "void PokePodLinkService::pollDeferredCleanup",
)
deferred_cleanup = body(
    "void PokePodLinkService::pollDeferredCleanup",
    "void PokePodLinkService::poll(uint32_t",
)

assert "manifestStepper_.active()" in poll
assert "advanceManifest(nowMs)" in poll
assert "linkTransferPermitted(transferGate_, nowMs)" in advance
assert advance.count("transferPermitted()") >= 2
assert "kLinkManifestMaximumReadBytes" in hash_step
assert "kLinkMaxDataBytes" in hash_step
assert "acquireIo(" in hash_step
assert "StorageAccess::read, 0" in hash_step
assert "if (!transferPermitted()) return" in hash_step
assert "finishManifestResponse" in SERVICE
assert response.index("manifestStepper_.items()") < response.index(
    "manifestResponseJson_ = response"
)
assert "sendJson(requestId, response)" in pending_response
assert "cleanupManifestStorage()" in SERVICE
assert "StorageAccess::read, 0" in cleanup
assert "if (!lease)" in cleanup
assert cleanup.index("if (!lease)") < cleanup.index("manifestFile_.close()")
assert "abortManifest();" in disconnect
assert "if (manifestCleanupPending_) cleanupManifestStorage()" in deferred_cleanup
wireless_service = (ROOT / "firmware/PokePodAmoled/WirelessSyncService.cpp").read_text()
wireless_poll_start = wireless_service.index("void WirelessSyncService::poll")
wireless_deadline = wireless_service.index("enforceDeadline(nowMs);", wireless_poll_start)
assert wireless_service.index("link_.pollDeferredCleanup();", wireless_poll_start) < wireless_deadline
assert "manifestRequestId_ == requestId && manifestStepper_.active()" in SERVICE
assert "MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT" in HEADER
assert "kLinkManifestMaximumReadBytes = 16U * 1024U" in HEADER
assert "kLinkManifestMaximumFiles = 2048" in HEADER
assert 'manifestError_ = "manifest file limit exceeded"' in SERVICE
assert "metadataFingerprint()" not in SERVICE
assert "sha256File(" not in SERVICE

print("PASS test_link_manifest_contract")
