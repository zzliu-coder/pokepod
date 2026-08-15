#include "PokePodLinkService.h"

#include <cJSON.h>
#include <mbedtls/sha256.h>
#include <algorithm>
#include <vector>

#include "AudioPipeline.h"
#include "BoardServices.h"
#include "CapsuleLibrary.h"
#include "CapabilityRegistry.h"
#include "DeviceConfig.h"
#include "DeviceRebootCoordinator.h"
#include "FontPolicy.h"
#include "ProvisioningCoordinator.h"
#include "TencentWorker.h"
#include "WifiController.h"
#include "WirelessSyncPairing.h"
#include "WirelessSyncProtocol.h"

namespace pokepod {
namespace {

constexpr uint32_t kMaxIncomingCommandBytes = 1024 * 1024;
constexpr size_t kMaxCommandJsonBytes = 64U * 1024U;
constexpr size_t kReadPageFiles = 12;
constexpr const char *kLinkCommandUploadPart =
    "/PokeCapsule/.system/commands/incoming/upload.part";
constexpr const char *kLinkStagedUploadPart =
    "/PokeCapsule/.staging/link-upload.part";
constexpr const char *kLinkFontUploadPart =
    "/PokeCapsule/.system/fonts/cjk20.a4.part";

cJSON *asJson(void *value) { return static_cast<cJSON *>(value); }

const char *jsonString(cJSON *root, const char *name) {
  cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
  return cJSON_IsString(item) && item->valuestring != nullptr
      ? item->valuestring : nullptr;
}

bool jsonBool(cJSON *root, const char *name, bool fallback = false) {
  cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
  return cJSON_IsBool(item) ? cJSON_IsTrue(item) : fallback;
}

int64_t jsonInt64(cJSON *root, const char *name, int64_t fallback = -1) {
  cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
  return cJSON_IsNumber(item) ? static_cast<int64_t>(item->valuedouble)
                              : fallback;
}

String printed(cJSON *root) {
  char *value = cJSON_PrintUnformatted(root);
  const String result = value == nullptr ? String() : String(value);
  cJSON_free(value);
  return result;
}

bool hiddenReadDenied(const String &relative) {
  return relative == ".staging" || relative.startsWith(".staging/") ||
         relative == ".system" || relative.startsWith(".system/") ||
         relative == ".commands" || relative.startsWith(".commands/");
}

String parentPath(const String &path) {
  const int slash = path.lastIndexOf('/');
  return slash <= 0 ? String("/") : path.substring(0, slash);
}

uint16_t requiredCapabilitiesForLinkOperation(const char *operation) {
  if (operation == nullptr) return 0;
  if (strcmp(operation, "record") == 0 ||
      strcmp(operation, "stop") == 0) {
    return kRecordingCapabilities;
  }
  if (strcmp(operation, "font-write") == 0) {
    return capabilityMask(DeviceCapability::storage);
  }
  if (strcmp(operation, "fingerprint") == 0 ||
      strcmp(operation, "read") == 0 ||
      strcmp(operation, "stage-write") == 0 ||
      strcmp(operation, "command") == 0 ||
      strcmp(operation, "commit") == 0 ||
      strcmp(operation, "result") == 0) {
    return kCapsuleBrowsingCapabilities;
  }
  return 0;
}

}  // namespace

void PokePodLinkService::processRequest(uint32_t requestId,
                                        const uint8_t *payload, size_t size) {
  if (requestId == 0 || completed_.contains(requestId)) {
    sendError(requestId, requestId == 0 ? "requestId must be non-zero" :
                                         "duplicate requestId");
    return;
  }
  const LinkOperationAdmission admission = admitLinkOperation(requestId);
  if (admission == LinkOperationAdmission::sameRequest) return;
  if (admission == LinkOperationAdmission::busy) {
    sendBusy(requestId);
    return;
  }
  if (admission != LinkOperationAdmission::accepted) {
    sendError(requestId, "request admission failed");
    return;
  }
  cJSON *root = cJSON_ParseWithLength(
      reinterpret_cast<const char *>(payload), size);
  const char *operation = root == nullptr ? nullptr : jsonString(root, "operation");
  if (root == nullptr || !cJSON_IsObject(root) || operation == nullptr ||
      jsonInt64(root, "version") != kLinkVersion) {
    cJSON_Delete(root);
    sendError(requestId, "malformed Link v2 request");
    return;
  }

  const uint16_t required = requiredCapabilitiesForLinkOperation(operation);
  if (required != 0 && capabilities_ != nullptr &&
      !capabilities_->allows(required)) {
    cJSON_Delete(root);
    sendError(requestId, "required device capability is not ready");
    return;
  }

  const int64_t binaryLength = jsonInt64(root, "binaryLength", 0);
  const bool chunkAcks = jsonBool(root, "chunkAcks");
  if (strcmp(operation, "font-write") == 0) {
    cJSON_Delete(root);
    const String finalPath = String(kCapsuleSystem) + "/fonts/cjk20.a4";
    const String temporaryPath = kLinkFontUploadPart;
    if (foregroundBusy()) {
      sendBusy(requestId);
    } else if (binaryLength < 20 || binaryLength > 5 * 1024 * 1024 ||
               !ensureDirectoryTree(parentPath(finalPath)) ||
               !beginIncoming(IncomingKind::systemFont, requestId,
                              static_cast<uint32_t>(binaryLength),
                              temporaryPath, finalPath, "", chunkAcks)) {
      sendError(requestId, "invalid font transfer");
    }
    return;
  }
  if (strcmp(operation, "firmware-update") == 0) {
    const char *expectedSha256 = jsonString(root, "sha256");
    const bool usbTransport = transport_ == LinkTransport::usb;
    const bool valid = usbTransport && rebootCoordinator_ != nullptr &&
        !foregroundBusy() && !rebootCoordinator_->pending() &&
        FirmwareUpdatePolicy::validImageSize(
            static_cast<uint32_t>(std::max<int64_t>(0, binaryLength))) &&
        FirmwareUpdatePolicy::validSha256(expectedSha256);
    if (!valid) {
      cJSON_Delete(root);
      sendError(requestId, usbTransport ? "invalid firmware update request" :
                                         "firmware update is USB-only");
      return;
    }
    const uint32_t expectedBytes = static_cast<uint32_t>(binaryLength);
    const bool started = firmwareUpdate_.begin(expectedBytes, expectedSha256);
    const String error = firmwareUpdate_.error();
    cJSON_Delete(root);
    if (!started) {
      sendError(requestId, error.isEmpty() ? "OTA begin failed" : error.c_str());
      return;
    }
    firmwareUpdateRequestId_ = requestId;
    operation_.advance(LinkOperationState::receiving);
    operation_.ownResource(LinkOperationResource::firmwareUpdate);
    return;
  }
  if (strcmp(operation, "stage-write") == 0 || strcmp(operation, "command") == 0) {
    if (foregroundBusy()) {
      cJSON_Delete(root);
      sendBusy(requestId);
      return;
    }
    const char *transactionId = jsonString(root, "transactionId");
    if (!isUuid(transactionId) || binaryLength < 0 ||
        binaryLength > static_cast<int64_t>(kMaxIncomingCommandBytes * 3U)) {
      cJSON_Delete(root);
      sendError(requestId, "invalid transaction or binary length");
      return;
    }
    if (strcmp(operation, "stage-write") == 0) {
      const char *capsuleId = jsonString(root, "capsuleId");
      const char *path = jsonString(root, "path");
      if (!isUuid(capsuleId) || !safeLinkRelativePath(path) ||
          binaryLength > 3LL * 1024LL * 1024LL) {
        cJSON_Delete(root);
        sendError(requestId, "invalid staged file target");
        return;
      }
      const String directory = String(kCapsuleStaging) + "/" + transactionId +
          "/" + capsuleId;
      const String finalPath = directory + "/" + path;
      const String temporaryPath = kLinkStagedUploadPart;
      cJSON_Delete(root);
      if (!ensureDirectoryTree(parentPath(finalPath)) ||
          !beginIncoming(IncomingKind::stagedFile, requestId,
                         static_cast<uint32_t>(binaryLength), temporaryPath,
                         finalPath, transactionId, chunkAcks)) {
        sendError(requestId, "cannot open staged file");
      }
      return;
    }
    if (binaryLength > static_cast<int64_t>(kMaxCommandJsonBytes)) {
      cJSON_Delete(root);
      sendError(requestId, "command is too large");
      return;
    }
    const String finalPath = String(kCapsuleSystem) + "/commands/incoming/" +
        transactionId + ".json";
    const String temporaryPath = kLinkCommandUploadPart;
    cJSON_Delete(root);
    if (!beginIncoming(IncomingKind::command, requestId,
                       static_cast<uint32_t>(binaryLength), temporaryPath,
                       finalPath, transactionId, chunkAcks)) {
      sendError(requestId, "cannot open command file");
    }
    return;
  }

  handleImmediate(requestId, root);
  cJSON_Delete(root);
  if (manifestRequestId_ == requestId && manifestStepper_.active()) return;
  if (recordingSession_.ownsRequest(requestId)) return;
}

void PokePodLinkService::handleImmediate(uint32_t requestId, void *jsonRoot) {
  cJSON *root = asJson(jsonRoot);
  const char *operation = jsonString(root, "operation");
  if (strcmp(operation, "hello") == 0) {
    const char *capabilities = transport_ == LinkTransport::usb
        ? "\"protocol\":\"PokePod Link\",\"capabilities\":[\"read\",\"stage-write\",\"command\",\"configure\",\"set-time\",\"record\",\"stop\",\"font-write\",\"firmware-update\",\"provisioning-diagnostics\",\"power-diagnostics\",\"runtime-diagnostics\",\"clear-runtime-diagnostics\",\"provisioning-start\",\"provisioning-stop\",\"pairing-export\",\"reboot\"]"
        : "\"protocol\":\"PokePod Link\",\"capabilities\":[\"read\",\"stage-write\",\"command\",\"configure\",\"set-time\",\"record\",\"stop\",\"font-write\",\"provisioning-diagnostics\",\"power-diagnostics\",\"runtime-diagnostics\",\"clear-runtime-diagnostics\",\"reboot\"]";
    sendOk(requestId, capabilities);
  } else if (strcmp(operation, "status") == 0) {
    (void)sendTerminalOrDisconnect(requestId, diagnostics_.statusJson());
  } else if (strcmp(operation, "provisioning-start") == 0) {
    if (transport_ != LinkTransport::usb ||
        provisioningCoordinator_ == nullptr) {
      sendError(requestId,
                "provisioning-start is only available over USB");
    } else if (foregroundBusy()) {
      sendBusy(requestId);
    } else if (!provisioningCoordinator_->request(millis())) {
      sendError(requestId, "provisioning startup request rejected");
    } else {
      const String extra = "\"phase\":\"" +
          String(provisioningCoordinator_->phaseName()) + "\"";
      sendOk(requestId, extra.c_str());
    }
  } else if (strcmp(operation, "provisioning-stop") == 0) {
    if (transport_ != LinkTransport::usb ||
        provisioningCoordinator_ == nullptr) {
      sendError(requestId,
                "provisioning-stop is only available over USB");
    } else {
      provisioningCoordinator_->stop();
      sendOk(requestId, "\"phase\":\"idle\"");
    }
  } else if (strcmp(operation, "get-provisioning-diagnostics") == 0) {
    sendJson(requestId, diagnostics_.provisioningJson());
  } else if (strcmp(operation, "clear-provisioning-diagnostics") == 0) {
    if (foregroundBusy()) sendBusy(requestId);
    else if (diagnostics_.clearProvisioning(*log_)) sendOk(requestId);
    else sendError(requestId, "provisioning diagnostics clear failed");
  } else if (strcmp(operation, "get-power-diagnostics") == 0) {
    sendJson(requestId, diagnostics_.powerJson());
  } else if (strcmp(operation, "clear-power-diagnostics") == 0) {
    if (foregroundBusy()) sendBusy(requestId);
    else if (diagnostics_.clearPower(*log_)) sendOk(requestId);
    else sendError(requestId, "power diagnostics clear failed");
  } else if (strcmp(operation, "get-runtime-diagnostics") == 0) {
    sendJson(requestId, diagnostics_.runtimeJson());
  } else if (strcmp(operation, "clear-runtime-diagnostics") == 0) {
    if (foregroundBusy()) sendBusy(requestId);
    else if (diagnostics_.clearRuntime(*log_)) sendOk(requestId);
    else sendError(requestId, "runtime diagnostics clear failed");
  } else if (strcmp(operation, "identity") == 0) {
    const String extra = "\"deviceId\":\"" + deviceId() +
        "\",\"displayName\":\"PokePod\",\"platform\":\"pokepod\",\"manufacturer\":\"PokeCapsule\",\"model\":\"" +
        String(variantName(board_->status().variant)) + "\"";
    sendOk(requestId, extra.c_str());
  } else if (strcmp(operation, "pairing-export") == 0) {
    if (transport_ != LinkTransport::usb || pairingProvider_ == nullptr) {
      sendError(requestId, "pairing-export is only available over USB");
    } else {
      String bundle;
      String error;
      if (pairingProvider_->pairingBundle(jsonBool(root, "rotate"),
                                          bundle, error)) {
        const std::string encoded = base64UrlEncode(
            reinterpret_cast<const uint8_t *>(bundle.c_str()),
            bundle.length());
        const String extra = "\"bundle\":\"" + String(encoded.c_str()) + "\"";
        sendOk(requestId, extra.c_str());
      } else {
        sendError(requestId, error.c_str());
      }
    }
  } else if (strcmp(operation, "fingerprint") == 0) {
    if (foregroundBusy()) sendBusy(requestId);
    else beginManifest(requestId, LinkManifestMode::fingerprint);
  } else if (strcmp(operation, "read") == 0) {
    if (foregroundBusy()) sendBusy(requestId);
    else handleRead(requestId, root);
  } else if (strcmp(operation, "commit") == 0) {
    const char *transactionId = jsonString(root, "transactionId");
    const char *capsuleId = jsonString(root, "capsuleId");
    const String path = String(kCapsuleStaging) + "/" +
        (transactionId == nullptr ? "" : transactionId) + "/" +
        (capsuleId == nullptr ? "" : capsuleId);
    if (foregroundBusy()) sendBusy(requestId);
    else if (!isUuid(transactionId) || !isUuid(capsuleId) ||
             !storageExists(path, StorageAccess::read)) {
      sendError(requestId, "staging transaction is incomplete");
    } else sendOk(requestId, "\"staged\":true");
  } else if (strcmp(operation, "result") == 0) {
    const char *transactionId = jsonString(root, "transactionId");
    if (!isUuid(transactionId)) {
      sendError(requestId, "invalid transactionId");
      return;
    }
    const String path = String(kCapsuleSystem) + "/commands/results/" +
        transactionId + ".json";
    if (!storageExists(path, StorageAccess::read)) {
      sendOk(requestId, "\"available\":false");
    } else {
      fileTransfer_.send(requestId, path, transactionId);
    }
  } else if (strcmp(operation, "configure") == 0) {
    if (foregroundBusy()) sendBusy(requestId);
    else handleConfigure(requestId, root);
  } else if (strcmp(operation, "set-time") == 0) {
    const int64_t unixTimeMs = jsonInt64(root, "unixTimeMs");
    if (unixTimeMs < 1704067200000LL || !board_->setUtcEpoch(unixTimeMs / 1000)) {
      sendError(requestId, "invalid UTC time");
    } else sendOk(requestId);
  } else if (strcmp(operation, "record") == 0) {
    if (foregroundBusy()) {
      sendBusy(requestId);
    } else if ((capabilities_ != nullptr &&
                !capabilities_->allows(kRecordingCapabilities)) ||
               audio_ == nullptr || !audio_->ready() || !board_->sdReady() ||
               !recordingSession_.ready()) {
      sendError(requestId, "recording capability is not ready");
    } else {
      const String id = newUuid();
      tencent_->wake();
      uint32_t sessionId = esp_random();
      if (sessionId == 0) sessionId = 1;
      const RecorderOperationOwner recorderOwner =
          transport_ == LinkTransport::wifi
              ? RecorderOperationOwner::linkWifi
              : RecorderOperationOwner::linkUsb;
      const LinkRecordingRequestResult result = recordingSession_.requestStart(
          requestId, id, sessionId, board_->utcNow(), recorderOwner,
          operation_, transactionGate_, transferGate_);
      if (result.status == LinkRecordingRequestStatus::busy) {
        sendBusy(requestId);
      } else if (result.status == LinkRecordingRequestStatus::failed) {
        sendError(requestId, result.message);
      }
      // accepted and cleanupPending complete asynchronously through poll().
    }
  } else if (strcmp(operation, "stop") == 0) {
    if (!recordingSession_.recordingActive()) {
      sendError(requestId, "recording is not active");
    } else if (!recordingSession_.requestStop(
                   requestId, true, true, operationOwns(requestId), operation_,
                   transactionGate_)) {
      sendBusy(requestId);
    }
  } else if (strcmp(operation, "reboot") == 0) {
    if (rebootCoordinator_ == nullptr) {
      sendError(requestId, "reboot coordinator is unavailable");
    } else if (rebootCoordinator_->pending()) {
      sendBusy(requestId);
    } else if (sendOk(requestId)) {
      (void)rebootCoordinator_->request(millis(), transport_);
    }
  } else {
    sendError(requestId, "unsupported Link v2 operation");
  }
}

void PokePodLinkService::handleRead(uint32_t requestId, void *jsonRoot) {
  cJSON *root = asJson(jsonRoot);
  const char *requested = jsonString(root, "path");
  if (requested == nullptr) {
    sendError(requestId, "read path is required");
    return;
  }
  if (strcmp(requested, ".") == 0 && jsonBool(root, "recursive")) {
    const char *cursorText = jsonString(root, "cursor");
    size_t cursor = cursorText == nullptr ? 0 : strtoul(cursorText, nullptr, 10);
    beginManifest(requestId, LinkManifestMode::recursiveRead, cursor);
    return;
  }
  const String relative(requested);
  if (!safeLinkRelativePath(requested, true) || hiddenReadDenied(relative)) {
    sendError(requestId, "unsafe read path");
    return;
  }
  fileTransfer_.send(requestId, String(kCapsuleRoot) + "/" + relative);
}

bool PokePodLinkService::beginManifest(uint32_t requestId,
                                       LinkManifestMode mode,
                                       size_t cursor) {
  if (manifestStepper_.active() || fileTransfer_.active() ||
      txStepper_.active() || pendingControlBytes_ != 0) {
    sendBusy(requestId);
    return false;
  }
  StorageReservation reservation = StorageCoordinator::instance().reserve(
      storageOwner(), StorageAccess::read, 0);
  if (!reservation) {
    sendBusy(requestId);
    return false;
  }
  if (!manifestStepper_.begin(mode, cursor, kReadPageFiles)) {
    sendError(requestId, "manifest computation is busy");
    return false;
  }
  manifestStorageReservation_ = std::move(reservation);
  manifestDirectories_.clear();
  manifestDirectories_.reserve(6);
  manifestRequestId_ = requestId;
  manifestRootOpened_ = false;
  manifestError_ = "manifest computation failed";
  if (operationOwns(requestId)) {
    operation_.advance(LinkOperationState::processing);
    operation_.ownResource(LinkOperationResource::storageReservation);
    operation_.ownResource(LinkOperationResource::file);
  }
  return true;
}

void PokePodLinkService::advanceManifest(uint32_t nowMs) {
  if (!linkTransferPermitted(transferGate_, nowMs) || !transferPermitted()) {
    disconnect();
    return;
  }
  switch (manifestStepper_.phase()) {
    case LinkManifestPhase::scanning:
      advanceManifestScan();
      break;
    case LinkManifestPhase::sorting:
      manifestStepper_.stepSort(48);
      break;
    case LinkManifestPhase::processing:
      advanceManifestFile();
      break;
    case LinkManifestPhase::hashing:
      advanceManifestHash();
      break;
    case LinkManifestPhase::complete:
      finishManifestResponse();
      break;
    case LinkManifestPhase::failed:
      failManifest(manifestError_.c_str());
      break;
    case LinkManifestPhase::cancelled:
      disconnect();
      break;
    case LinkManifestPhase::idle:
      break;
  }
  if (!transferPermitted()) disconnect();
}

void PokePodLinkService::advanceManifestScan() {
  if (fs_ == nullptr || !transferPermitted()) return;
  StorageIoLease lease = StorageCoordinator::instance().acquireIo(
      storageOwner(), StorageAccess::read, 0);
  if (!lease) return;

  if (!manifestRootOpened_) {
    File root = fs_->open(kCapsuleRoot);
    if (!root || !root.isDirectory()) {
      if (root) root.close();
      manifestError_ = "capsule root is missing";
      manifestStepper_.fail();
      return;
    }
    ManifestDirectoryCursor cursor;
    cursor.directory = root;
    cursor.absolute = kCapsuleRoot;
    cursor.relative = "";
    cursor.depth = 0;
    manifestDirectories_.push_back(cursor);
    manifestRootOpened_ = true;
    return;
  }

  if (manifestDirectories_.empty()) {
    if (!manifestStepper_.finishScan()) {
      manifestError_ = manifestStepper_.mode() ==
              LinkManifestMode::recursiveRead
          ? "invalid read cursor"
          : "manifest scan failed";
    }
    return;
  }

  ManifestDirectoryCursor &cursor = manifestDirectories_.back();
  File entry = cursor.directory.openNextFile();
  if (!entry) {
    cursor.directory.close();
    manifestDirectories_.pop_back();
    return;
  }
  const String full = entry.name();
  const bool directory = entry.isDirectory();
  const int slash = full.lastIndexOf('/');
  const String name = slash >= 0 ? full.substring(slash + 1) : full;
  const String relative = cursor.relative.isEmpty()
      ? name : cursor.relative + "/" + name;
  const String absolute = cursor.absolute + "/" + name;
  if (hiddenReadDenied(relative)) {
    entry.close();
    return;
  }
  if (directory) {
    if (cursor.depth >= 5) {
      entry.close();
      manifestError_ = "manifest directory depth exceeded";
      manifestStepper_.fail();
      return;
    }
    ManifestDirectoryCursor child;
    child.directory = entry;
    child.absolute = absolute;
    child.relative = relative;
    child.depth = cursor.depth + 1;
    manifestDirectories_.push_back(child);
    return;
  }
  entry.close();
  if (!manifestStepper_.addPath(relative)) {
    manifestError_ = "manifest file limit exceeded";
  }
}

void PokePodLinkService::advanceManifestFile() {
  if (fs_ == nullptr || !transferPermitted()) return;
  String relative;
  if (!manifestStepper_.currentPath(relative)) {
    manifestError_ = "manifest path state failed";
    manifestStepper_.fail();
    return;
  }
  if (manifestStepper_.mode() == LinkManifestMode::fingerprint &&
      !manifestStepper_.currentIsMetadata()) {
    manifestStepper_.skipCurrent();
    return;
  }

  StorageIoLease lease = StorageCoordinator::instance().acquireIo(
      storageOwner(), StorageAccess::read, 0);
  if (!lease) return;
  File file = fs_->open(String(kCapsuleRoot) + "/" + relative, FILE_READ);
  if (!file || file.isDirectory()) {
    if (file) file.close();
    manifestError_ = "manifest file is missing";
    manifestStepper_.fail();
    return;
  }
  const size_t length = file.size();
  if (manifestStepper_.mode() == LinkManifestMode::recursiveRead &&
      !linkAudioFileNeedsDigest(relative.c_str())) {
    file.close();
    if (!manifestStepper_.finishCurrentWithoutHash(length)) {
      manifestError_ = "manifest item failed";
      manifestStepper_.fail();
    }
    return;
  }

  if (!manifestStepper_.beginCurrentFile(length)) {
    file.close();
    manifestError_ = "manifest hash state failed";
    manifestStepper_.fail();
    return;
  }
  manifestFile_ = file;
  if (manifestStepper_.mode() == LinkManifestMode::recursiveRead) {
    mbedtls_sha256_init(&manifestSha_);
    manifestShaActive_ = true;
    if (mbedtls_sha256_starts(&manifestSha_, 0) != 0) {
      manifestError_ = "audio digest failed";
      manifestStepper_.fail();
    }
  }
}

void PokePodLinkService::advanceManifestHash() {
  if (!manifestFile_ || manifestFile_.isDirectory() ||
      !transferPermitted()) {
    manifestError_ = "manifest hash file failed";
    manifestStepper_.fail();
    return;
  }
  const size_t length = manifestFile_.size();
  const size_t hashed = manifestStepper_.currentHashedBytes();
  if (hashed > length) {
    manifestError_ = "manifest hash length failed";
    manifestStepper_.fail();
    return;
  }
  const size_t remaining = length - hashed;
  if (remaining > 0) {
    StorageIoLease lease = StorageCoordinator::instance().acquireIo(
        storageOwner(), StorageAccess::read, 0);
    if (!lease) return;
    const size_t wanted = std::min(
        remaining, std::min(kLinkManifestMaximumReadBytes,
                            static_cast<size_t>(kLinkMaxDataBytes)));
    const int received = manifestFile_.read(payload_, wanted);
    if (received <= 0 || static_cast<size_t>(received) > wanted) {
      manifestError_ = manifestStepper_.mode() ==
              LinkManifestMode::recursiveRead
          ? "audio digest failed"
          : "fingerprint read failed";
      manifestStepper_.fail();
      return;
    }
    if (manifestShaActive_ &&
        mbedtls_sha256_update(&manifestSha_, payload_, received) != 0) {
      manifestError_ = "audio digest failed";
      manifestStepper_.fail();
      return;
    }
    if (!manifestStepper_.acceptHashBytes(payload_, received)) {
      manifestError_ = "manifest hash budget failed";
      return;
    }
    if (!transferPermitted()) return;
  }
  if (manifestStepper_.currentHashedBytes() != length) return;

  StorageIoLease lease = StorageCoordinator::instance().acquireIo(
      storageOwner(), StorageAccess::read, 0);
  if (!lease) return;
  manifestFile_.close();
  if (manifestStepper_.mode() == LinkManifestMode::recursiveRead) {
    uint8_t digest[32] = {};
    const bool digestOk = manifestShaActive_ &&
        mbedtls_sha256_finish(&manifestSha_, digest) == 0;
    if (manifestShaActive_) mbedtls_sha256_free(&manifestSha_);
    manifestShaActive_ = false;
    if (!digestOk) {
      manifestError_ = "audio digest failed";
      manifestStepper_.fail();
      return;
    }
    static constexpr char kHex[] = "0123456789abcdef";
    char hex[65] = {};
    for (size_t index = 0; index < sizeof(digest); ++index) {
      hex[index * 2] = kHex[digest[index] >> 4];
      hex[index * 2 + 1] = kHex[digest[index] & 0x0f];
    }
    if (!manifestStepper_.finishCurrentFile(hex)) {
      manifestError_ = "audio digest commit failed";
    }
  } else if (!manifestStepper_.finishCurrentFile()) {
    manifestError_ = "fingerprint commit failed";
  }
}

void PokePodLinkService::finishManifestResponse() {
  const uint32_t requestId = manifestRequestId_;
  String response;
  if (manifestStepper_.mode() == LinkManifestMode::recursiveRead) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "ok");
    cJSON_AddNumberToObject(root, "version", kLinkVersion);
    cJSON *items = cJSON_AddArrayToObject(root, "files");
    for (const LinkManifestItem &manifestItem : manifestStepper_.items()) {
      cJSON *item = cJSON_CreateObject();
      cJSON_AddStringToObject(item, "path", manifestItem.path.c_str());
      cJSON_AddNumberToObject(item, "length", manifestItem.length);
      if (!manifestItem.sha256.isEmpty()) {
        cJSON_AddStringToObject(item, "sha256",
                                manifestItem.sha256.c_str());
      }
      cJSON_AddItemToArray(items, item);
    }
    if (manifestStepper_.hasNextPage()) {
      cJSON_AddStringToObject(root, "nextCursor",
                              String(manifestStepper_.nextCursor()).c_str());
    }
    response = printed(root);
    cJSON_Delete(root);
  } else {
    char fingerprint[24] = {};
    snprintf(fingerprint, sizeof(fingerprint), "%016llx",
             static_cast<unsigned long long>(
                 manifestStepper_.fingerprint()));
    response = "{\"status\":\"ok\",\"version\":2,\"fingerprint\":\"" +
        String(fingerprint) + "\"}";
  }

  manifestResponseRequestId_ = requestId;
  manifestResponseJson_ = response;
  abortManifest();
  if (!manifestCleanupPending_) finishPendingManifestResponse();
}

void PokePodLinkService::failManifest(const char *message) {
  const bool permitted = transferPermitted();
  manifestFailureRequestId_ = permitted ? manifestRequestId_ : 0;
  manifestFailureMessage_ = message == nullptr
      ? "manifest computation failed" : message;
  abortManifest();
  if (!permitted) {
    disconnect();
    return;
  }
  if (!manifestCleanupPending_) finishPendingManifestFailure();
}

void PokePodLinkService::abortManifest() {
  if (manifestShaActive_) {
    mbedtls_sha256_free(&manifestSha_);
    manifestShaActive_ = false;
  }
  manifestStepper_.cancel();
  manifestRequestId_ = 0;
  manifestRootOpened_ = false;
  manifestError_ = "";
  manifestCleanupPending_ = !cleanupManifestStorage();
}

bool PokePodLinkService::cleanupManifestStorage() {
  bool hasOpenHandle = static_cast<bool>(manifestFile_);
  if (!hasOpenHandle) {
    for (const ManifestDirectoryCursor &cursor : manifestDirectories_) {
      if (cursor.directory) {
        hasOpenHandle = true;
        break;
      }
    }
  }
  StorageIoLease lease;
  if (hasOpenHandle || manifestStorageReservation_) {
    lease = StorageCoordinator::instance().acquireIo(
        storageOwner(), StorageAccess::read, 0);
    if (!lease) {
      manifestCleanupPending_ = true;
      return false;
    }
  }
  if (manifestFile_) manifestFile_.close();
  for (ManifestDirectoryCursor &cursor : manifestDirectories_) {
    if (cursor.directory) cursor.directory.close();
  }
  manifestDirectories_.clear();
  manifestStorageReservation_.release();
  operation_.releaseResource(LinkOperationResource::file);
  operation_.releaseResource(LinkOperationResource::storageReservation);
  manifestStepper_.reset();
  manifestCleanupPending_ = false;
  return true;
}

void PokePodLinkService::finishPendingManifestResponse() {
  const uint32_t requestId = manifestResponseRequestId_;
  const String response = manifestResponseJson_;
  manifestResponseRequestId_ = 0;
  manifestResponseJson_ = "";
  if (requestId == 0 || !transferPermitted() ||
      !sendJson(requestId, response)) {
    disconnect();
    return;
  }
}

void PokePodLinkService::finishPendingManifestFailure() {
  const uint32_t requestId = manifestFailureRequestId_;
  const String message = manifestFailureMessage_;
  manifestFailureRequestId_ = 0;
  manifestFailureMessage_ = "";
  if (requestId == 0 || !transferPermitted()) {
    disconnect();
    return;
  }
  sendError(requestId, message.c_str());
}

void PokePodLinkService::handleConfigure(uint32_t requestId, void *jsonRoot) {
  cJSON *root = asJson(jsonRoot);
  cJSON *values = cJSON_GetObjectItemCaseSensitive(root, "values");
  if (!cJSON_IsObject(values)) {
    sendError(requestId, "configuration values are required");
    return;
  }
  DeviceSettings settings = config_->settings();
  struct StringField { const char *wire; String *target; };
  const StringField fields[] = {
      {"wifiSsid", &settings.wifiSsid}, {"wifiPassword", &settings.wifiPassword},
      {"secretId", &settings.secretId}, {"secretKey", &settings.secretKey},
      {"hotwordId", &settings.hotwordId},
  };
  for (const StringField &field : fields) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(values, field.wire);
    if (cJSON_IsString(item) && item->valuestring != nullptr) *field.target = item->valuestring;
    else if (cJSON_IsNull(item)) *field.target = "";
    else if (item != nullptr) {
      sendError(requestId, "invalid configuration value type");
      return;
    }
  }
  cJSON *wifiEnabled = cJSON_GetObjectItemCaseSensitive(values, "wifiEnabled");
  cJSON *raiseToWake = cJSON_GetObjectItemCaseSensitive(values, "raiseToWake");
  if (cJSON_IsBool(wifiEnabled)) settings.wifiEnabled = cJSON_IsTrue(wifiEnabled);
  if (cJSON_IsBool(raiseToWake)) settings.raiseToWake = cJSON_IsTrue(raiseToWake);
  if (!config_->save(settings, *log_)) {
    sendError(requestId, "configuration save failed");
    return;
  }
  wifi_->configurationChanged();
  tencent_->wake();
  const String extra = "\"wifiConfigured\":" +
      String(config_->hasWifi() ? "true" : "false") +
      ",\"wifiNetworkCount\":" +
      String(static_cast<unsigned>(config_->wifiNetworks().size())) +
      ",\"tencentConfigured\":" +
      String(config_->hasTencent() ? "true" : "false");
  sendOk(requestId, extra.c_str());
}

}  // namespace pokepod
