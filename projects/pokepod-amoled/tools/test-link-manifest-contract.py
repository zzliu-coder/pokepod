#!/usr/bin/env python3

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SERVICE = (ROOT / "firmware/PokePodAmoled/PokePodLinkService.cpp").read_text()
DISPATCHER = (ROOT / "firmware/PokePodAmoled/LinkCommandDispatcher.cpp").read_text()
TRANSPORT = (ROOT / "firmware/PokePodAmoled/LinkTransportSession.cpp").read_text()
COMBINED = SERVICE + DISPATCHER + TRANSPORT
HEADER = (ROOT / "firmware/PokePodAmoled/LinkManifestStepper.h").read_text()


def body(source: str, signature: str, following: str) -> str:
    start = source.index(signature)
    end = source.index(following, start)
    return source[start:end]


poll = body(TRANSPORT, "void PokePodLinkService::poll", "void PokePodLinkService::consumeByte")
advance = body(
    DISPATCHER, "void PokePodLinkService::advanceManifest",
    "void PokePodLinkService::advanceManifestScan",
)
hash_step = body(
    DISPATCHER, "void PokePodLinkService::advanceManifestHash",
    "void PokePodLinkService::finishManifestResponse",
)
response = body(
    DISPATCHER, "void PokePodLinkService::finishManifestResponse",
    "void PokePodLinkService::failManifest",
)
pending_response = body(
    DISPATCHER, "void PokePodLinkService::finishPendingManifestResponse",
    "void PokePodLinkService::finishPendingManifestFailure",
)
cleanup = body(
    DISPATCHER, "bool PokePodLinkService::cleanupManifestStorage",
    "void PokePodLinkService::finishPendingManifestResponse",
)
disconnect = body(
    TRANSPORT, "void PokePodLinkService::disconnect",
    "void PokePodLinkService::pollDeferredCleanup",
)
deferred_cleanup = body(
    TRANSPORT, "bool PokePodLinkService::pollDeferredCleanup",
    "void PokePodLinkService::pollDeferredCleanup",
)
deferred_cleanup_wrapper = body(
    TRANSPORT, "void PokePodLinkService::pollDeferredCleanup",
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
assert "finishManifestResponse" in COMBINED
assert response.index("manifestStepper_.items()") < response.index(
    "manifestResponseJson_ = response"
)
assert "sendJson(requestId, response)" in pending_response
assert "cleanupManifestStorage()" in COMBINED
assert "StorageAccess::read, 0" in cleanup
assert "if (!lease)" in cleanup
assert cleanup.index("if (!lease)") < cleanup.index("manifestFile_.close()")
assert "abortManifest();" in disconnect
assert "manifestCleanupPending_" in deferred_cleanup
assert "cleanupManifestStorage()" in deferred_cleanup
assert "LinkPollPhaseGate gate(" in deferred_cleanup_wrapper
wireless_service = (ROOT / "firmware/PokePodAmoled/WirelessSyncService.cpp").read_text()
wireless_poll_start = wireless_service.index("void WirelessSyncService::poll")
wireless_deadline = wireless_service.index("enforceDeadline(nowMs);", wireless_poll_start)
assert wireless_service.index("link_.pollDeferredCleanup();", wireless_poll_start) < wireless_deadline
assert "manifestRequestId_ == requestId && manifestStepper_.active()" in COMBINED
assert "MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT" in HEADER
assert "kLinkManifestMaximumReadBytes = 16U * 1024U" in HEADER
assert "kLinkManifestMaximumFiles = 2048" in HEADER
assert 'manifestError_ = "manifest file limit exceeded"' in COMBINED
assert "metadataFingerprint()" not in COMBINED
assert "sha256File(" not in COMBINED

print("PASS test_link_manifest_contract")
