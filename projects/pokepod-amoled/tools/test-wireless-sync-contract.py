#!/usr/bin/env python3
import base64
import hashlib
import hmac
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "firmware" / "PokePodAmoled"


def read(name: str) -> str:
    return (SOURCE / name).read_text(encoding="utf-8")


pairing_id = "6dc5d4b6-2618-49a4-94fb-874cfb0c6d81"
client_nonce = "ICEiIyQlJicoKSorLC0uLw"
server_nonce = "MDEyMzQ1Njc4OTo7PD0-Pw"
secret = base64.urlsafe_b64decode(
    "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8" + "="
)


def proof(role: str) -> str:
    canonical = (
        f"pokecapsule-v1|{role}|{pairing_id}|{client_nonce}|{server_nonce}"
    ).encode()
    digest = hmac.new(secret, canonical, hashlib.sha256).digest()
    return base64.urlsafe_b64encode(digest).decode().rstrip("=")


assert proof("server") == "mZNFax3mBwLRI59z64lyeAsLGga-vGoa18gdNF-qnaM"
assert proof("client") == "fvsCMDq_IGNdIDRQYaVoVOicK98-Q7n-dV_NWa-a1B8"

bonjour = read("WirelessSyncBonjour.cpp")
for key in ("schema", "v", "instance", "paired", "caps"):
    assert f'"{key}"' in bonjour
assert '"link-v2,tls,hmac-sha256,resume"' in bonjour
for forbidden in ("deviceId", "displayName", "certificateSha256", "secret"):
    assert forbidden not in bonjour

tls = read("WirelessSyncTlsStream.cpp")
assert "esp_tls_server_session_init" in tls
assert "esp_tls_server_session_continue_async" in tls
assert "esp_tls_conn_read" in tls and "esp_tls_conn_write" in tls
assert tls.count("ensureTransferPermitted()") >= 8
assert "if (!ensureTransferPermitted()) return offset;" in tls
assert tls.index("if (!ensureTransferPermitted()) return offset;") < tls.index(
    "esp_tls_conn_write"
)
assert "public LinkTransferCancellationSink" in read("WirelessSyncTlsStream.h")
assert "cancelForTransferDeadline()" in tls
assert "transferGate_->attachCancellationSink(this)" in tls
assert "transferGate_->detachCancellationSink(this)" in tls

service = read("WirelessSyncService.cpp")
poll_turn = read("WirelessLinkPollTurn.h")
assert "LinkTransport::wifi, nullptr" in service
assert "link_.begin(tls_" in service
assert "link_.begin(incoming" not in service
assert "&window_.transferGate()" in service
assert "window_.transferGate(), nowMs" in service
assert "authenticationDeadline_.observeTlsReady(nowMs);" in service
assert service.index("if (!tls_.ready()) return;") < service.index(
    "authenticationDeadline_.observeTlsReady(nowMs);"
)
assert service.index("authenticationDeadline_.expired(nowMs)") < service.index(
    "authenticator_.poll(tls_)"
)
assert "clientStartedAtMs_" not in service
assert "WirelessLinkPollTurn<PokePodLinkService> linkTurn(link_);" in service
assert "linkTurn.pollAuthenticated(nowMs);" in service
assert "link_.pollDeferredCleanup();" not in service
assert "link_.poll(nowMs);" not in service
assert "if (!fullPollRan_) link_.pollDeferredCleanup();" in poll_turn
assert "fullPollRan_ = true;" in poll_turn
assert poll_turn.index("fullPollRan_ = true;") < poll_turn.index(
    "link_.poll(nowMs);"
)

window = read("WirelessSyncWindow.h")
assert "kWirelessSyncWindowMs = 5UL * 60UL * 1000UL" in window
assert "kWirelessSyncDrainLimitMs" not in window
assert "draining" not in window
assert "atomicTransactionActive" not in window
assert "deadlineReached" in window
main = read("PokePodApp.cpp")
assert main.index("wirelessSync.enforceDeadline(now);") < main.index(
    "if (linkService.receivingBinary())"
)

link = read("PokePodLinkService.cpp")
dispatcher = read("LinkCommandDispatcher.cpp")
link_recording = read("LinkRecordingSession.cpp")
link_surface = link + dispatcher
assert 'strcmp(operation, "pairing-export") == 0' in dispatcher
assert "transport_ != LinkTransport::usb" in dispatcher
assert "transport_ == LinkTransport::usb" in dispatcher
assert "base64UrlEncode(" in dispatcher
assert '\\"bundle\\"' in dispatcher
assert "wireless-pairing-bundle" not in link_surface
assert 'strcmp(basename, "audio.m4a") == 0' in read("LinkPolicy.h")
assert 'strcmp(basename, "audio.wav") == 0' in read("LinkPolicy.h")
assert 'cJSON_AddStringToObject(item, "sha256",' in dispatcher
assert "manifestItem.sha256.c_str()" in dispatcher
assert "mbedtls_sha256_starts" in dispatcher
assert "mbedtls_sha256_update" in dispatcher
assert "mbedtls_sha256_finish" in dispatcher
assert 'static constexpr char kHex[] = "0123456789abcdef"' in dispatcher
assert "char hex[65] = {}" in dispatcher
assert link_surface.count("transferPermitted()") >= 12
assert "AudioCaptureRuntime *captureRuntime" in read("PokePodLinkService.h")
assert "recorder_->requestStart(" in link_recording
assert "recorderOwner);" in link_recording
assert "RecordingSpaceSnapshot" not in link_surface
assert "RecorderOperationOwner::linkWifi" in dispatcher
assert "RecorderOperationOwner::linkUsb" in dispatcher
assert "recorder_->pollStart" in link_recording
assert "captureRuntime_->start(*audio_, captureSessionId, *log_)" in link_recording
assert "recorder_->start(*log_, id, board_->utcNow())" not in link_surface
assert "audio_->startCapture(*log_)" not in link_surface
assert "captureRuntime_->stop(*log_)" in link_recording

main = read("PokePodApp.cpp")
assert "LinkTransport::usb, &wirelessSync,\n                    nullptr" in main

identity = read("WirelessSyncIdentity.cpp")
assert "rotationPolicy_.shouldRotate(rotate" in identity
assert 'cJSON_AddStringToObject(root, "deviceId", deviceId_)' in identity
assert 'cJSON_AddStringToObject(root, "platform", "pokepod")' in identity
for field in (
    "schemaVersion",
    "kind",
    "pairingId",
    "deviceId",
    "displayName",
    "platform",
    "secret",
    "certificateSha256",
    "serviceType",
    "expiresAt",
):
    assert f'"{field}"' in identity

identity_blob = read("WirelessSyncIdentityBlob.h")
assert '"pokepod-%04x%08x"' in identity_blob
assert "formatPokePodDeviceUuid" not in identity_blob
assert "formatPokePodDeviceId(ESP.getEfuseMac(), value)" in link
assert "formatPokePodDeviceId(hardwareId, deviceId_)" in identity
assert "formatPokePodDeviceId(ESP.getEfuseMac(), value)" in main

print("PASS wireless_sync_contract")
