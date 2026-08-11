#include "PokePodLinkService.h"

#include "WifiFailurePolicy.h"

#include <SD_MMC.h>
#include <cJSON.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <mbedtls/sha256.h>
#include <algorithm>
#include <vector>

#include "AudioPipeline.h"
#include "AudioCaptureRuntime.h"
#include "AudioCaptureRouter.h"
#include "BoardServices.h"
#include "CapsuleLibrary.h"
#include "CapsulePolicy.h"
#include "LinkCommandBatchPolicy.h"
#include "DeviceConfig.h"
#include "Dashboard.h"
#include "FontPolicy.h"
#include "TencentWorker.h"
#include "UsbLinkBridge.h"
#include "BleVoiceService.h"
#include "ProvisioningDiagnostics.h"
#include "PowerDiagnostics.h"
#include "ProvisioningCoordinator.h"
#include "RuntimePowerManager.h"
#include "WavRecorder.h"
#include "WifiController.h"
#include "WirelessSyncIdentityBlob.h"
#include "WirelessSyncPairing.h"
#include "WirelessSyncProtocol.h"

namespace pokepod {
namespace {

constexpr uint32_t kMaxIncomingCommandBytes = 1024 * 1024;
constexpr size_t kMaxCommandJsonBytes = kMaxIncomingCommandBytes;
constexpr size_t kReadPageFiles = 12;
constexpr size_t kLinkTxFrameBytes = kLinkHeaderBytes + kLinkMaxDataBytes;
constexpr size_t kLinkPendingControlBytes =
    kLinkHeaderBytes + kLinkMaxControlBytes;
constexpr size_t kLinkWriteSliceBytes = 512;

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
  return cJSON_IsNumber(item) ? static_cast<int64_t>(item->valuedouble) : fallback;
}

String printed(cJSON *root) {
  char *value = cJSON_PrintUnformatted(root);
  const String result = value == nullptr ? String() : String(value);
  cJSON_free(value);
  return result;
}

String jsonEscaped(const String &value) {
  String escaped;
  escaped.reserve(value.length() + 8);
  for (size_t index = 0; index < value.length(); ++index) {
    const char character = value[index];
    switch (character) {
      case '\\': escaped += "\\\\"; break;
      case '"': escaped += "\\\""; break;
      case '\n': escaped += "\\n"; break;
      case '\r': escaped += "\\r"; break;
      case '\t': escaped += "\\t"; break;
      default:
        if (static_cast<uint8_t>(character) >= 0x20) escaped += character;
        break;
    }
  }
  return escaped;
}

void replaceStringOrNull(cJSON *root, const char *name, const char *value) {
  cJSON *replacement = value == nullptr ? cJSON_CreateNull()
                                         : cJSON_CreateString(value);
  if (cJSON_HasObjectItem(root, name)) {
    cJSON_ReplaceItemInObjectCaseSensitive(root, name, replacement);
  } else {
    cJSON_AddItemToObject(root, name, replacement);
  }
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

bool sameUuid(const char *left, const char *right) {
  return left != nullptr && right != nullptr && isUuid(left) && isUuid(right) &&
         strcasecmp(left, right) == 0;
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

bool PokePodLinkService::begin(Stream &stream, fs::FS &fs,
                               BoardServices &board, AudioPipeline &audio,
                               AudioCaptureRouter &captureRouter,
                               UsbLinkBridge &usb, BleVoiceService &bleVoice,
                               Dashboard &dashboard,
                               CapsuleLibrary &library, WavRecorder &recorder,
                               DeviceConfig &config, WifiController &wifi,
                               TencentWorker &tencent,
                               ProvisioningDiagnostics &provisioningDiagnostics,
                               PowerDiagnostics &powerDiagnostics,
                               RuntimePowerManager &power, Print &log,
                               LinkServiceCoordinator *coordinator,
                               LinkTransport transport,
                               WirelessSyncPairingProvider *pairingProvider,
                               LinkTransferGate *transferGate,
                               ProvisioningCoordinator *provisioningCoordinator,
                               LinkWriteChannel *writeChannel,
                               AudioCaptureRuntime *captureRuntime,
                               const CapabilityRegistry *capabilities) {
  stream_ = &stream;
  fs_ = &fs;
  board_ = &board;
  audio_ = &audio;
  captureRuntime_ = captureRuntime;
  capabilities_ = capabilities;
  captureRouter_ = &captureRouter;
  usb_ = &usb;
  bleVoice_ = &bleVoice;
  dashboard_ = &dashboard;
  library_ = &library;
  recorder_ = &recorder;
  config_ = &config;
  wifi_ = &wifi;
  tencent_ = &tencent;
  provisioningDiagnostics_ = &provisioningDiagnostics;
  powerDiagnostics_ = &powerDiagnostics;
  provisioningCoordinator_ = provisioningCoordinator;
  power_ = &power;
  log_ = &log;
  coordinator_ = coordinator;
  transport_ = transport;
  pairingProvider_ = pairingProvider;
  transferGate_ = transferGate;
  writeChannel_ = writeChannel;
  requestLeaseHeld_ = false;
  activeMaintenance_ = "";
  if (!transaction_.begin(fs, log)) return false;
  if (payload_ == nullptr) {
    payload_ = static_cast<uint8_t *>(heap_caps_calloc(
        kLinkMaxDataBytes, sizeof(uint8_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  }
  if (txFrame_ == nullptr) {
    txFrame_ = static_cast<uint8_t *>(heap_caps_calloc(
        kLinkTxFrameBytes, sizeof(uint8_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  }
  if (pendingControlFrame_ == nullptr) {
    pendingControlFrame_ = static_cast<uint8_t *>(heap_caps_calloc(
        kLinkPendingControlBytes, sizeof(uint8_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  }
  if (payload_ == nullptr || txFrame_ == nullptr ||
      pendingControlFrame_ == nullptr) {
    stream_ = nullptr;
    log_->println(
        "{\"event\":\"link_buffer\",\"ok\":false,\"memory\":\"psram\"}");
    return false;
  }
  log_->printf(
      "{\"event\":\"link_buffer\",\"ok\":true,\"memory\":\"psram\",\"bytes\":%u}\n",
      static_cast<unsigned>(kLinkMaxDataBytes + kLinkTxFrameBytes +
                            kLinkPendingControlBytes));
  if (!ensureDirectoryTree(String(kCapsuleSystem) + "/commands/results") ||
      !ensureDirectoryTree(String(kCapsuleSystem) + "/commands/incoming")) {
    return false;
  }
  if (!cleanupPurgeStaging()) {
    log_->println("{\"event\":\"purge_cleanup_deferred\"}");
  }
  return true;
}

void PokePodLinkService::disconnect() {
  manifestResponseRequestId_ = 0;
  manifestResponseJson_ = "";
  manifestFailureRequestId_ = 0;
  manifestFailureMessage_ = "";
  if (linkOwnedRecording_) stopLinkRecording(false);
  abortManifest();
  abortOutgoing();
  sessionActive_ = false;
  incomingCleanupRespond_ = false;
  if (incomingKind_ != IncomingKind::none || incomingCleanupPending_) {
    failIncoming("transport disconnected");
  }
  activeMaintenance_ = "";
  maintenanceCompletion_.disconnect();
  completed_.clear();
  resetFrame();
  releaseRequestLeaseNow();
}

void PokePodLinkService::pollDeferredCleanup() {
  if (incomingCleanupPending_) cleanupIncomingStorage();
  if (outgoingCleanupPending_) cleanupOutgoingStorage();
  if (manifestCleanupPending_) cleanupManifestStorage();
  stepDeferredFileCleanup();
  if (deferredCommandFiles_.empty()) stepDeferredTreeCleanup();
  finishCommandStorageCleanup();
}

void PokePodLinkService::poll(uint32_t nowMs) {
  pollDeferredCleanup();
  if (incomingCleanupPending_ || outgoingCleanupPending_ ||
      manifestCleanupPending_ || !deferredCommandFiles_.empty() ||
      !deferredTreeCleanupStack_.empty()) return;
  if (stream_ == nullptr) return;
  if (manifestResponseRequestId_ != 0) {
    finishPendingManifestResponse();
    return;
  }
  if (manifestFailureRequestId_ != 0) {
    finishPendingManifestFailure();
    return;
  }
  if (linkOwnedRecording_ && !drainLinkCapture()) {
    stopLinkRecording(false);
  }
  if (!transferPermitted()) {
    disconnect();
    return;
  }
  if (txStepper_.active()) {
    advanceTransmit(nowMs);
    return;
  }
  if (outgoingPhase_ == OutgoingPhase::data) {
    queueNextFileChunk();
    return;
  }
  if (manifestStepper_.active() || manifestStepper_.complete() ||
      manifestStepper_.failed()) {
    advanceManifest(nowMs);
    return;
  }
  frameProcessedThisPoll_ = false;
  size_t budget = 32768;
  while (!frameProcessedThisPoll_ && !txStepper_.active() && transferPermitted() &&
         stream_->available() > 0 && budget-- > 0) {
    const int value = stream_->read();
    if (value >= 0) consumeByte(static_cast<uint8_t>(value));
  }
  // A request can start after the caller captured nowMs. Subtracting that
  // older timestamp from the freshly recorded byte time underflows uint32_t
  // and used to reject every upload immediately on some loop iterations.
  if (incomingKind_ != IncomingKind::none &&
      static_cast<uint32_t>(millis() - incomingLastByteMs_) > 5000) {
    failIncoming("binary transfer timed out");
  }
  if (rebootAtMs_ != 0 && static_cast<int32_t>(nowMs - rebootAtMs_) >= 0) {
    if (tencent_ != nullptr &&
        !tencent_->quiesce(nowMs, 250, TencentCancelReason::shutdown)) {
      rebootAtMs_ = millis() + 100;
      return;
    }
    if (stream_ != nullptr) stream_->flush();
    ESP.restart();
  }
}

void PokePodLinkService::consumeByte(uint8_t value) {
  if (receivePhase_ == ReceivePhase::magic) {
    static constexpr uint8_t magic[4] = {'P', 'P', 'V', '2'};
    if (value == magic[magicMatched_]) {
      headerBytes_[magicMatched_++] = value;
      if (magicMatched_ == 4) {
        sessionActive_ = true;
        headerUsed_ = 4;
        receivePhase_ = ReceivePhase::header;
      }
    } else {
      magicMatched_ = value == 'P' ? 1 : 0;
      if (magicMatched_ == 1) headerBytes_[0] = value;
    }
    return;
  }
  if (receivePhase_ == ReceivePhase::header) {
    headerBytes_[headerUsed_++] = value;
    if (headerUsed_ < kLinkHeaderBytes) return;
    if (!decodeLinkHeader(headerBytes_, sizeof(headerBytes_), currentHeader_)) {
      const uint32_t requestId = linkGet32(headerBytes_ + 8);
      resetFrame();
      if (requestId != 0) sendError(requestId, "invalid Link v2 header");
      return;
    }
    payloadUsed_ = 0;
    receivePhase_ = ReceivePhase::payload;
    if (currentHeader_.payloadLength == 0) processFrame();
    return;
  }
  if (payload_ != nullptr && payloadUsed_ < kLinkMaxDataBytes) {
    payload_[payloadUsed_++] = value;
  }
  if (payloadUsed_ == currentHeader_.payloadLength) processFrame();
}

void PokePodLinkService::resetFrame() {
  receivePhase_ = ReceivePhase::magic;
  headerUsed_ = 0;
  payloadUsed_ = 0;
  magicMatched_ = 0;
  currentHeader_ = LinkFrameHeader();
}

void PokePodLinkService::processFrame() {
  frameProcessedThisPoll_ = true;
  const LinkFrameHeader header = currentHeader_;
  const size_t size = payloadUsed_;
  if (!transferPermitted()) {
    resetFrame();
    return;
  }
  if (!validateLinkPayload(header, payload_, size)) {
    resetFrame();
    sendError(header.requestId, "Link v2 CRC mismatch");
    return;
  }
  resetFrame();
  if (header.type == LinkFrameType::requestJson) {
    processRequest(header.requestId, payload_, size);
  } else if (header.type == LinkFrameType::data) {
    processData(header.requestId, header.flags, payload_, size);
  } else {
    sendError(header.requestId, "unexpected Link v2 frame type");
  }
}

void PokePodLinkService::processRequest(uint32_t requestId,
                                        const uint8_t *payload, size_t size) {
  if (requestId == 0 || completed_.contains(requestId) ||
      incomingKind_ != IncomingKind::none) {
    sendError(requestId, requestId == 0 ? "requestId must be non-zero" :
              (completed_.contains(requestId) ? "duplicate requestId" :
                                                "another request is receiving data"));
    return;
  }
  cJSON *root = cJSON_ParseWithLength(
      reinterpret_cast<const char *>(payload), size);
  const char *operation = root == nullptr ? nullptr : jsonString(root, "operation");
  if (root == nullptr || !cJSON_IsObject(root) || operation == nullptr ||
      jsonInt64(root, "version") != kLinkVersion) {
    cJSON_Delete(root);
    sendError(requestId, "malformed Link v2 request");
    rememberCompleted(requestId);
    return;
  }

  const uint16_t required = requiredCapabilitiesForLinkOperation(operation);
  if (required != 0 && capabilities_ != nullptr &&
      !capabilities_->allows(required)) {
    cJSON_Delete(root);
    sendError(requestId, "required device capability is not ready");
    rememberCompleted(requestId);
    return;
  }

  const int64_t binaryLength = jsonInt64(root, "binaryLength", 0);
  const bool chunkAcks = jsonBool(root, "chunkAcks");
  if (strcmp(operation, "font-write") == 0) {
    if (!acquireRequestLease(requestId)) {
      cJSON_Delete(root);
      rememberCompleted(requestId);
      return;
    }
    cJSON_Delete(root);
    const String finalPath = String(kCapsuleSystem) + "/fonts/cjk20.a4";
    const String temporaryPath = finalPath + ".part";
    if (foregroundBusy()) {
      sendBusy(requestId);
      rememberCompleted(requestId);
      releaseRequestLease();
    } else if (binaryLength < 20 || binaryLength > 5 * 1024 * 1024 ||
               !ensureDirectoryTree(parentPath(finalPath)) ||
               !beginIncoming(IncomingKind::systemFont, requestId,
                              static_cast<uint32_t>(binaryLength),
                              temporaryPath, finalPath, "", chunkAcks)) {
      sendError(requestId, "invalid font transfer");
      rememberCompleted(requestId);
      releaseRequestLease();
    }
    return;
  }
  if (strcmp(operation, "stage-write") == 0 || strcmp(operation, "command") == 0) {
    if (!acquireRequestLease(requestId)) {
      cJSON_Delete(root);
      rememberCompleted(requestId);
      return;
    }
    if (foregroundBusy()) {
      cJSON_Delete(root);
      sendBusy(requestId);
      rememberCompleted(requestId);
      releaseRequestLease();
      return;
    }
    const char *transactionId = jsonString(root, "transactionId");
    if (!isUuid(transactionId) || binaryLength < 0 ||
        binaryLength > static_cast<int64_t>(kMaxIncomingCommandBytes * 3U)) {
      cJSON_Delete(root);
      sendError(requestId, "invalid transaction or binary length");
      rememberCompleted(requestId);
      releaseRequestLease();
      return;
    }
    if (strcmp(operation, "stage-write") == 0) {
      const char *capsuleId = jsonString(root, "capsuleId");
      const char *path = jsonString(root, "path");
      if (!isUuid(capsuleId) || !safeLinkRelativePath(path) ||
          binaryLength > 3LL * 1024LL * 1024LL) {
        cJSON_Delete(root);
        sendError(requestId, "invalid staged file target");
        rememberCompleted(requestId);
        releaseRequestLease();
        return;
      }
      const String directory = String(kCapsuleStaging) + "/" + transactionId +
          "/" + capsuleId;
      const String finalPath = directory + "/" + path;
      const String temporaryPath = finalPath + ".part";
      cJSON_Delete(root);
      if (!ensureDirectoryTree(parentPath(finalPath)) ||
          !beginIncoming(IncomingKind::stagedFile, requestId,
                         static_cast<uint32_t>(binaryLength), temporaryPath,
                         finalPath, transactionId, chunkAcks)) {
        sendError(requestId, "cannot open staged file");
        rememberCompleted(requestId);
        releaseRequestLease();
      }
      return;
    }
    if (binaryLength > static_cast<int64_t>(kMaxIncomingCommandBytes)) {
      cJSON_Delete(root);
      sendError(requestId, "command is too large");
      rememberCompleted(requestId);
      releaseRequestLease();
      return;
    }
    const String finalPath = String(kCapsuleSystem) + "/commands/incoming/" +
        transactionId + ".json";
    const String temporaryPath = finalPath + ".part";
    cJSON_Delete(root);
    if (!beginIncoming(IncomingKind::command, requestId,
                       static_cast<uint32_t>(binaryLength), temporaryPath,
                       finalPath, transactionId, chunkAcks)) {
      sendError(requestId, "cannot open command file");
      rememberCompleted(requestId);
      releaseRequestLease();
    }
    return;
  }

  if (acquireRequestLease(requestId)) handleImmediate(requestId, root);
  cJSON_Delete(root);
  if (manifestRequestId_ == requestId && manifestStepper_.active()) return;
  rememberCompleted(requestId);
  if (activeMaintenance_.isEmpty()) releaseRequestLease();
}

bool PokePodLinkService::beginIncoming(IncomingKind kind, uint32_t requestId,
                                       uint32_t expectedBytes,
                                       const String &temporaryPath,
                                       const String &finalPath,
                                       const String &transactionId,
                                       bool chunkAcks) {
  incomingStorageReservation_ = StorageCoordinator::instance().reserve(
      storageOwner(), StorageAccess::mutation, storageIoTimeout());
  if (!incomingStorageReservation_) return false;
  StorageIoLease lease = StorageCoordinator::instance().acquireIo(
      storageOwner(), StorageAccess::mutation, storageIoTimeout());
  if (!lease) {
    incomingStorageReservation_.release();
    return false;
  }
  if (fs_->exists(temporaryPath)) fs_->remove(temporaryPath);
  File output = fs_->open(temporaryPath, FILE_WRITE);
  if (!output) {
    incomingStorageReservation_.release();
    return false;
  }
  incomingKind_ = kind;
  incomingRequestId_ = requestId;
  incomingExpected_ = expectedBytes;
  incomingReceived_ = 0;
  incomingChunkAcks_ = chunkAcks;
  incomingFile_ = output;
  incomingTemporaryPath_ = temporaryPath;
  incomingFinalPath_ = finalPath;
  incomingTransactionId_ = transactionId;
  incomingLastByteMs_ = millis();
  return true;
}

void PokePodLinkService::processData(uint32_t requestId, uint16_t flags,
                                     const uint8_t *payload, size_t size) {
  if (incomingKind_ == IncomingKind::none || requestId != incomingRequestId_) {
    sendError(requestId, "unexpected data frame");
    return;
  }
  StorageIoLease writeIo = StorageCoordinator::instance().acquireIo(
      storageOwner(), StorageAccess::mutation, storageIoTimeout());
  if (!writeIo || incomingReceived_ + size > incomingExpected_ ||
      (size > 0 && incomingFile_.write(payload, size) != size)) {
    failIncoming("staged write failed");
    return;
  }
  incomingReceived_ += size;
  incomingLastByteMs_ = millis();
  const bool last = (flags & 1U) != 0;
  if (last && incomingReceived_ != incomingExpected_) {
    failIncoming("binary length mismatch");
    return;
  }
  if (incomingChunkAcks_) {
    sendEvent(requestId, "{\"event\":\"binary_ack\",\"received\":" +
        String(incomingReceived_) + "}");
  }
  if (last) finishIncoming();
}

void PokePodLinkService::finishIncoming() {
  const IncomingKind kind = incomingKind_;
  const uint32_t requestId = incomingRequestId_;
  const String temporary = incomingTemporaryPath_;
  const String finalPath = incomingFinalPath_;
  const String transaction = incomingTransactionId_;
  StorageIoLease finishIo = StorageCoordinator::instance().acquireIo(
      storageOwner(), StorageAccess::mutation, storageIoTimeout());
  if (!finishIo) {
    failIncoming("storage busy");
    return;
  }
  incomingFile_.flush();
  const bool writeOk = incomingFile_.getWriteError() == 0;
  incomingFile_.close();
  incomingKind_ = IncomingKind::none;
  incomingRequestId_ = 0;
  incomingExpected_ = incomingReceived_ = 0;
  incomingChunkAcks_ = false;
  incomingTemporaryPath_ = incomingFinalPath_ = incomingTransactionId_ = "";
  incomingLastByteMs_ = 0;
  const bool valid = writeOk &&
      (kind != IncomingKind::systemFont || validFontFile(temporary));
  finishIo.release();
  const bool committed = valid && transaction_.commitPreparedFile(
      transaction.isEmpty() ? finalPath.c_str() : transaction.c_str(),
      temporary, finalPath, storageOwner());
  if (!committed) {
    incomingCleanupRespond_ = true;
    incomingCleanupRequestId_ = requestId;
    incomingCleanupMessage_ = "atomic file commit failed";
    if (!incomingCleanup_.begin(incomingFile_, fs_, temporary,
                                incomingStorageReservation_, storageOwner(),
                                StorageAccess::mutation)) return;
    incomingCleanupPending_ = true;
    cleanupIncomingStorage();
    return;
  }
  incomingStorageReservation_.release();
  if (kind == IncomingKind::command) {
    handleCommandFile(requestId, finalPath, transaction);
    if (commandCleanupPending_) return;
  } else if (kind == IncomingKind::systemFont) {
    sendOk(requestId, "\"installed\":true,\"rebootRequired\":true");
  } else {
    sendOk(requestId);
  }
  rememberCompleted(requestId);
  if (activeMaintenance_.isEmpty()) releaseRequestLease();
}

void PokePodLinkService::failIncoming(const char *message) {
  if (!incomingCleanupPending_) {
    incomingCleanupRequestId_ = incomingRequestId_;
    incomingCleanupMessage_ = message == nullptr ? "staged transfer failed" :
                                                    message;
    incomingCleanupRespond_ = sessionActive_ && transferPermitted();
    incomingCleanupPending_ = incomingCleanup_.begin(
        incomingFile_, fs_, incomingTemporaryPath_,
        incomingStorageReservation_, storageOwner(),
        StorageAccess::mutation);
  }
  cleanupIncomingStorage();
}

bool PokePodLinkService::cleanupIncomingStorage() {
  if (!incomingCleanupPending_) return true;
  if (!incomingCleanup_.poll()) return false;
  incomingCleanupPending_ = false;
  finishIncomingCleanup();
  return true;
}

void PokePodLinkService::finishIncomingCleanup() {
  const uint32_t requestId = incomingCleanupRequestId_;
  const String message = incomingCleanupMessage_;
  const bool respond = incomingCleanupRespond_ && sessionActive_ &&
      transferPermitted();
  incomingKind_ = IncomingKind::none;
  incomingRequestId_ = 0;
  incomingExpected_ = incomingReceived_ = 0;
  incomingChunkAcks_ = false;
  incomingTemporaryPath_ = incomingFinalPath_ = incomingTransactionId_ = "";
  incomingLastByteMs_ = 0;
  incomingCleanupRequestId_ = 0;
  incomingCleanupMessage_ = "";
  incomingCleanupRespond_ = false;
  if (respond && requestId != 0) {
    sendError(requestId, message.c_str());
    rememberCompleted(requestId);
    if (activeMaintenance_.isEmpty()) releaseRequestLease();
  }
}

void PokePodLinkService::handleImmediate(uint32_t requestId, void *jsonRoot) {
  cJSON *root = asJson(jsonRoot);
  const char *operation = jsonString(root, "operation");
  if (strcmp(operation, "hello") == 0) {
    const char *capabilities = transport_ == LinkTransport::usb
        ? "\"protocol\":\"PokePod Link\",\"capabilities\":[\"read\",\"stage-write\",\"command\",\"configure\",\"set-time\",\"record\",\"stop\",\"font-write\",\"provisioning-diagnostics\",\"power-diagnostics\",\"provisioning-start\",\"provisioning-stop\",\"pairing-export\",\"reboot\"]"
        : "\"protocol\":\"PokePod Link\",\"capabilities\":[\"read\",\"stage-write\",\"command\",\"configure\",\"set-time\",\"record\",\"stop\",\"font-write\",\"provisioning-diagnostics\",\"power-diagnostics\",\"reboot\"]";
    sendOk(requestId, capabilities);
  } else if (strcmp(operation, "status") == 0) {
    const BoardStatus &status = board_->status();
    String extra = "\"recording\":";
    extra += recorder_->recording() ? "true" : "false";
    extra += ",\"transcribing\":";
    extra += tencent_->working() ? "true" : "false";
    extra += ",\"batteryPercent\":" + String(status.batteryPercent);
    extra += ",\"charging\":";
    extra += status.charging ? "true" : "false";
    extra += ",\"wifi\":\"" + String(wifi_->phaseName()) + "\"";
    extra += ",\"wifiDisconnectReason\":" +
        String(wifi_->lastDisconnectReason());
    extra += ",\"wifiDisconnectKind\":\"" +
        String(wifiFailureKey(wifi_->lastDisconnectReason())) + "\"";
    extra += ",\"wifiRadioOn\":" +
        String(wifi_->radioOn() ? "true" : "false");
    extra += ",\"wifiPowerSave\":" +
        String(wifi_->powerSaveEnabled() ? "true" : "false");
    extra += ",\"wifiPowerSaveError\":" +
        String(wifi_->powerSaveError());
    extra += ",\"pendingCapsules\":" + String(library_->pendingCount());
    extra += ",\"asr_hash_ms\":" + String(tencent_->lastHashElapsedMs());
    extra += ",\"asr_connect_ms\":" + String(tencent_->lastConnectElapsedMs());
    extra += ",\"asr_upload_ms\":" + String(tencent_->lastUploadElapsedMs());
    extra += ",\"asr_total_ms\":" + String(tencent_->lastTotalElapsedMs());
    extra += ",\"asr_last_code\":\"" + jsonEscaped(tencent_->lastCode()) + "\"";
    extra += ",\"asr_tls_error\":" + String(tencent_->lastNetworkError());
    extra += ",\"asr_tls_detail\":\"" +
        jsonEscaped(tencent_->lastNetworkErrorDetail()) + "\"";
    extra += ",\"asr_heap_free_before_tls\":" +
        String(tencent_->lastInternalHeapFreeBeforeTls());
    extra += ",\"asr_heap_largest_before_tls\":" +
        String(tencent_->lastInternalHeapLargestBeforeTls());
    extra += ",\"asr_psram_free_before_tls\":" +
        String(tencent_->lastPsramFreeBeforeTls());
    extra += ",\"sdReady\":";
    extra += status.sdCard ? "true" : "false";
    if (capabilities_ != nullptr) {
      extra += ",\"capabilityObservedMask\":" +
          String(capabilities_->observedMask());
      extra += ",\"capabilityReadyMask\":" +
          String(capabilities_->readyMask());
      extra += ",\"capsuleLibraryReady\":" +
          String(capabilities_->ready(DeviceCapability::capsuleLibrary)
                     ? "true" : "false");
      extra += ",\"recorderReady\":" +
          String(capabilities_->ready(DeviceCapability::recording)
                     ? "true" : "false");
      extra += ",\"asrWorkerReady\":" +
          String(capabilities_->ready(DeviceCapability::transcription)
                     ? "true" : "false");
    }
    extra += ",\"variant\":\"" + String(variantName(status.variant)) + "\"";
    extra += ",\"ioExpander\":" + String(status.ioExpander ? "true" : "false");
    extra += ",\"display\":" + String(status.display ? "true" : "false");
    extra += ",\"touch\":" + String(status.touch ? "true" : "false");
    extra += ",\"rtc\":" + String(status.rtc ? "true" : "false");
    extra += ",\"imu\":" + String(status.imu ? "true" : "false");
    extra += ",\"pmu\":" + String(status.pmu ? "true" : "false");
    extra += ",\"vbusPresent\":" + String(status.vbusPresent ? "true" : "false");
    extra += ",\"screenOn\":" + String(status.screenOn ? "true" : "false");
    extra += ",\"audio\":" + String(audio_ != nullptr && audio_->ready() ? "true" : "false");
    extra += ",\"usb\":" + String(usb_->ready() ? "true" : "false");
    extra += ",\"host_connected\":" + String(usb_->hostConnected() ? "true" : "false");
    extra += ",\"bleVoiceConnected\":" +
        String(bleVoice_ != nullptr && bleVoice_->connected() ? "true" : "false");
    extra += ",\"bleVoiceReady\":" +
        String(bleVoice_ != nullptr && bleVoice_->appReady() ? "true" : "false");
    extra += ",\"bleVoiceMtu\":" +
        String(bleVoice_ == nullptr ? 0 : bleVoice_->mtu());
    const BleVoiceQualitySnapshot bleQuality = bleVoice_ == nullptr
        ? BleVoiceQualitySnapshot() : bleVoice_->quality();
    extra += ",\"bleVoiceNotifyAttempts\":" +
        String(bleQuality.notifyAttempts);
    extra += ",\"bleVoiceNotifyAccepted\":" +
        String(bleQuality.notifyAccepted);
    extra += ",\"bleVoiceNotifyFailures\":" +
        String(bleQuality.notifyFailures);
    extra += ",\"bleVoiceQueueOverflows\":" +
        String(bleQuality.queueOverflows);
    extra += ",\"bleVoiceSessionFailures\":" +
        String(bleQuality.sessionFailures);
    extra += ",\"bleVoiceReadyTimeouts\":" +
        String(bleQuality.readyTimeouts);
    extra += ",\"bleVoiceStopAckTimeouts\":" +
        String(bleQuality.stopAckTimeouts);
    extra += ",\"bleVoiceStreamTimeouts\":" +
        String(bleQuality.streamTimeouts);
    extra += ",\"bleVoiceLastErrorCode\":" +
        String(bleQuality.lastErrorCode);
    extra += ",\"audio_read_bytes\":" + String(static_cast<unsigned long>(audio_->bytesRead()));
    extra += ",\"audio_read_failures\":" + String(audio_->readFailures());
    extra += ",\"audio_peak\":" + String(audio_->peakSample());
    extra += ",\"playback_last_error\":\"" +
        String(audio_->lastPlaybackError()) + "\"";
    extra += ",\"playback_start_failures\":" +
        String(audio_->playbackStartFailures());
    extra += ",\"playback_heap_largest_before_start\":" +
        String(audio_->playbackHeapLargestBeforeStart());
    extra += ",\"playback_file_reads\":" +
        String(audio_->playbackFileReadCount());
    extra += ",\"playback_pump_count\":" +
        String(audio_->playbackPumpCount());
    extra += ",\"playback_max_file_read_us\":" +
        String(audio_->playbackMaxFileReadUs());
    const AudioFrontEndMetrics &frontEnd = recorder_->audioMetrics();
    extra += ",\"audio_frontend_channel\":\"" +
        String(audioInputChannelName(frontEnd.selectedChannel)) + "\"";
    extra += ",\"audio_frontend_left_peak\":" + String(frontEnd.leftPeak);
    extra += ",\"audio_frontend_right_peak\":" + String(frontEnd.rightPeak);
    extra += ",\"audio_frontend_output_peak\":" + String(frontEnd.outputPeak);
    extra += ",\"audio_frontend_noise_floor\":" +
        String(frontEnd.estimatedNoiseFloor);
    extra += ",\"audio_frontend_suppressed_samples\":" +
        String(frontEnd.suppressedSamples);
    extra += ",\"audio_frontend_limited_samples\":" +
        String(frontEnd.limitedSamples);
    extra += ",\"audio_frontend_max_gain_q12\":" +
        String(frontEnd.maximumGainQ12);
    extra += ",\"tencentConfigured\":" +
        String(config_->hasTencent() ? "true" : "false");
    extra += ",\"wifiNetworkCount\":" +
        String(static_cast<unsigned>(config_->wifiNetworks().size()));
    extra += ",\"ui_full_redraws\":" + String(dashboard_->fullRedrawCount());
    extra += ",\"ui_body_redraws\":" + String(dashboard_->bodyRedrawCount());
    extra += ",\"ui_partial_redraws\":" + String(dashboard_->partialRedrawCount());
    extra += ",\"ui_scroll_frame_last_us\":" +
        String(dashboard_->scrollFrameLastUs());
    extra += ",\"ui_scroll_frame_max_us\":" +
        String(dashboard_->scrollFrameMaxUs());
    extra += ",\"ui_scroll_compose_max_us\":" +
        String(dashboard_->scrollComposeMaxUs());
    extra += ",\"ui_scroll_transfer_max_us\":" +
        String(dashboard_->scrollTransferMaxUs());
    extra += ",\"ui_scroll_frames_over_budget\":" +
        String(dashboard_->scrollFramesOverBudget());
    extra += ",\"capsule_full_scans\":" +
        String(library_->fullScanCount());
    extra += ",\"capsule_incremental_refreshes\":" +
        String(library_->incrementalRefreshCount());
    extra += ",\"capsule_refresh_fallbacks\":" +
        String(library_->refreshFallbackCount());
    extra += ",\"capsule_last_scan_us\":" +
        String(library_->lastScanUs());
    extra += ",\"capsule_max_scan_us\":" +
        String(library_->maxScanUs());
    extra += ",\"ui_frame_buffer\":" +
        String(dashboard_->frameBufferReady() ? "true" : "false");
    extra += ",\"ui_animation_buffer\":" +
        String(dashboard_->animationBufferReady() ? "true" : "false");
    const RuntimePowerSnapshot &power = power_->snapshot();
    extra += ",\"powerMode\":\"" + String(powerModeName(power.mode)) + "\"";
    extra += ",\"cpuMhz\":" + String(power.cpuMhz);
    extra += ",\"powerTransitions\":" + String(power.transitions);
    extra += ",\"lightSleepAttempts\":" +
        String(power.lightSleepAttempts);
    extra += ",\"lightSleepCount\":" + String(power.lightSleepCount);
    extra += ",\"lightSleepFailures\":" +
        String(power.lightSleepFailures);
    extra += ",\"lightSleepMs\":" +
        String(static_cast<unsigned long>(power.lightSleepUs / 1000ULL));
    extra += ",\"deepSleepWakeCount\":" +
        String(power.deepSleepWakeCount);
    extra += ",\"deepSleepArmAttempts\":" +
        String(power.deepSleepArmAttempts);
    extra += ",\"deepSleepArmFailures\":" +
        String(power.deepSleepArmFailures);
    extra += ",\"wokeFromDeepSleep\":" +
        String(power.wokeFromDeepSleep ? "true" : "false");
    extra += ",\"deepSleepTouchWakeArmed\":" +
        String(power.deepSleepTouchWakeArmed ? "true" : "false");
    extra += ",\"lastWakeCause\":" + String(power.lastWakeCause);
    extra += ",\"wakeCauses\":" + String(power.wakeCauses);
    const PowerDiagnosticsSnapshot &powerDiagnostic =
        powerDiagnostics_->snapshot();
    extra += ",\"powerActiveFacts\":" +
        String(powerDiagnostic.activeFacts);
    extra += ",\"powerLightBlockers\":" +
        String(powerDiagnostic.lightBlockers);
    extra += ",\"powerDeepBlockers\":" +
        String(powerDiagnostic.deepBlockers);
    extra += ",\"powerCurrentBlockers\":" +
        String(powerDiagnostic.currentBlockers);
    extra += ",\"powerIdleMs\":" + String(powerDiagnostic.idleMs);
    extra += ",\"powerDiagnosticCount\":" +
        String(static_cast<unsigned>(powerDiagnostics_->count()));
    extra += ",\"powerDiagnosticPersistFailures\":" +
        String(powerDiagnostic.persistFailures);
    extra += ",\"powerAutomaticScreenWakes\":" +
        String(powerDiagnostic.automaticScreenWakes);
    extra += ",\"resetReason\":" +
        String(static_cast<unsigned>(esp_reset_reason()));
    extra += ",\"internalHeapFree\":" + String(static_cast<unsigned>(
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
    extra += ",\"internalHeapLargest\":" + String(static_cast<unsigned>(
        heap_caps_get_largest_free_block(
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
    extra += ",\"psramFree\":" +
        String(static_cast<unsigned>(ESP.getFreePsram()));
    extra += ",\"automaticPmSupported\":" +
        String(power.automaticPmSupported ? "true" : "false");
    extra += ",\"bleModemSleepSupported\":" +
        String(power.bleModemSleepSupported ? "true" : "false");
    extra += ",\"provisioningDiagnosticCount\":" +
        String(static_cast<unsigned>(provisioningDiagnostics_->count()));
    extra += ",\"provisioningStartupPhase\":\"" +
        String(provisioningCoordinator_ == nullptr ? "unavailable" :
               provisioningCoordinator_->phaseName()) + "\"";
    sendOk(requestId, extra.c_str());
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
    sendJson(requestId, provisioningDiagnosticsJson());
  } else if (strcmp(operation, "clear-provisioning-diagnostics") == 0) {
    if (foregroundBusy()) sendBusy(requestId);
    else if (provisioningDiagnostics_->clear(*log_)) sendOk(requestId);
    else sendError(requestId, "provisioning diagnostics clear failed");
  } else if (strcmp(operation, "get-power-diagnostics") == 0) {
    sendJson(requestId, powerDiagnosticsJson());
  } else if (strcmp(operation, "clear-power-diagnostics") == 0) {
    if (foregroundBusy()) sendBusy(requestId);
    else if (powerDiagnostics_->clear(*log_)) sendOk(requestId);
    else sendError(requestId, "power diagnostics clear failed");
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
      sendFile(requestId, path, transactionId);
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
    if (foregroundBusy()) sendBusy(requestId);
    else if ((capabilities_ != nullptr &&
              !capabilities_->allows(kRecordingCapabilities)) ||
             audio_ == nullptr || !audio_->ready() || !board_->sdReady() ||
             captureRuntime_ == nullptr || !captureRuntime_->ready()) {
      sendError(requestId, "recording capability is not ready");
    } else {
      const String id = newUuid();
      tencent_->wake();
      // AudioCaptureRouter deliberately allows the same logical owner to
      // re-enter. A Link request is a distinct session, so require the router
      // to be genuinely idle before acquiring it. This prevents a failed Link
      // start from releasing a UI-owned local recording.
      const bool acquired = captureRouter_->available() &&
          captureRouter_->acquire(AudioCaptureOwner::localCapsule);
      const RecordingSpaceSnapshot space = {
          SD_MMC.totalBytes(), SD_MMC.usedBytes(), SD_MMC.totalBytes() != 0};
      uint32_t sessionId = esp_random();
      if (sessionId == 0) sessionId = 1;
      const bool recorderStarted = acquired &&
          recorder_->start(*log_, id, board_->utcNow(), space);
      const bool captureStarted = recorderStarted &&
          captureRuntime_->start(*audio_, sessionId, *log_);
      if (!captureStarted) {
        if (recorder_->recording()) recorder_->abortCapture(*log_);
        if (acquired) {
          captureRouter_->release(AudioCaptureOwner::localCapsule);
        }
        if (acquired) sendError(requestId, "recording start failed");
        else sendBusy(requestId);
      } else {
        linkOwnedRecording_ = true;
        const String extra = "\"recording\":true,\"capsuleId\":\"" + id + "\"";
        sendOk(requestId, extra.c_str());
      }
    }
  } else if (strcmp(operation, "stop") == 0) {
    if (!recorder_->recording() || !linkOwnedRecording_) {
      sendError(requestId, "recording is not active");
    }
    else {
      const bool committed = stopLinkRecording(true);
      if (committed) {
        const bool refreshQueued = library_->requestScan();
        if (refreshQueued) {
          sendOk(requestId,
                 "\"recording\":false,\"queued\":true,"
                 "\"indexRefresh\":\"queued\"");
        } else {
          sendError(requestId, "recording committed but index refresh failed");
        }
      } else {
        sendError(requestId, "recording commit failed");
      }
    }
  } else if (strcmp(operation, "reboot") == 0) {
    sendOk(requestId);
    rebootAtMs_ = millis() + 100;
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
  sendFile(requestId, String(kCapsuleRoot) + "/" + relative);
}

bool PokePodLinkService::beginManifest(uint32_t requestId,
                                       LinkManifestMode mode,
                                       size_t cursor) {
  if (manifestStepper_.active() || outgoingPhase_ != OutgoingPhase::none ||
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
  rememberCompleted(requestId);
  if (activeMaintenance_.isEmpty()) releaseRequestLease();
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
  rememberCompleted(requestId);
  if (activeMaintenance_.isEmpty()) releaseRequestLease();
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

void PokePodLinkService::handleCommandFile(uint32_t requestId,
                                           const String &path,
                                           const String &transactionId) {
  commandStorageReservation_ = StorageCoordinator::instance().reserve(
      StorageOwner::capsuleTransaction, StorageAccess::mutation, storageIoTimeout());
  if (!commandStorageReservation_) {
    sendError(requestId, "storage busy");
    return;
  }
  commandStorageActive_ = true;
  String message;
  const String resultPath = String(kCapsuleSystem) + "/commands/results/" +
      transactionId + ".json";
  const bool accepted = storageExists(resultPath, StorageAccess::read) ||
      executeCommand(path, transactionId, message);
  storageRemove(path);
  if (!accepted) {
    log_->printf("{\"event\":\"link_command_failed\",\"transaction\":\"%s\",\"message\":\"%s\"}\n",
                 transactionId.c_str(), message.c_str());
  }
  if (!deferredCommandFiles_.empty() ||
      !deferredTreeCleanupStack_.empty()) {
    commandCleanupPending_ = true;
    commandCleanupRequestId_ = requestId;
    return;
  }
  commandStorageActive_ = false;
  commandStorageReservation_.release();
  sendOk(requestId, "\"accepted\":true");
}

void PokePodLinkService::finishCommandStorageCleanup() {
  if (!commandCleanupPending_ || !deferredCommandFiles_.empty() ||
      !deferredTreeCleanupStack_.empty()) return;
  const uint32_t requestId = commandCleanupRequestId_;
  if (deferredCommandFileFailed_ && log_ != nullptr) {
    log_->println("{\"event\":\"link_command_flush_failed\"}");
  }
  deferredCommandFileFailed_ = false;
  commandCleanupPending_ = false;
  commandCleanupRequestId_ = 0;
  commandStorageActive_ = false;
  commandStorageReservation_.release();
  if (sessionActive_ && transferPermitted()) {
    sendOk(requestId, "\"accepted\":true");
    rememberCompleted(requestId);
    if (activeMaintenance_.isEmpty()) releaseRequestLease();
  }
}

bool PokePodLinkService::safeFolder(const char *value,
                                    bool allowBuiltIn) const {
  if (value == nullptr) return false;
  if (allowBuiltIn && (strcmp(value, "Inbox") == 0 ||
                       strcmp(value, "Archive") == 0)) return true;
  const size_t length = strlen(value);
  if (length == 0 || length > 161 || value[0] == '.' || value[0] == '/' ||
      value[length - 1] == '/') return false;
  uint8_t segments = 1;
  size_t segmentLength = 0;
  for (size_t index = 0; index < length; ++index) {
    const unsigned char c = static_cast<unsigned char>(value[index]);
    if (c == '\\' || c < 0x20) return false;
    if (c == '/') {
      if (segmentLength == 0 || ++segments > 2 || value[index + 1] == '.') return false;
      segmentLength = 0;
    } else {
      ++segmentLength;
    }
  }
  return segmentLength > 0;
}

String PokePodLinkService::folderDirectory(const char *value) const {
  if (!safeFolder(value)) return String();
  if (strcmp(value, "Inbox") == 0) return String(kCapsuleInbox);
  if (strcmp(value, "Archive") == 0) return String(kCapsuleArchive);
  return String(kCapsuleRoot) + "/" + value;
}

String PokePodLinkService::activeCapsuleDirectory(const String &id) const {
  const CapsuleSummary *record = library_->find(id);
  return record == nullptr || record->readOnly ? String() : record->directory;
}

bool PokePodLinkService::collectCommandIds(void *jsonRoot,
                                           std::vector<String> &ids) const {
  cJSON *array = cJSON_GetObjectItemCaseSensitive(asJson(jsonRoot), "capsuleIds");
  if (!cJSON_IsArray(array) || cJSON_GetArraySize(array) > 500) return false;
  cJSON *item = nullptr;
  cJSON_ArrayForEach(item, array) {
    const char *value = cJSON_GetStringValue(item);
    if (!isUuid(value)) return false;
    String normalized(value);
    normalized.toLowerCase();
    if (std::find(ids.begin(), ids.end(), normalized) != ids.end()) return false;
    ids.push_back(normalized);
  }
  return true;
}

bool PokePodLinkService::validateExpectedRevisions(
    void *jsonRoot, const std::vector<String> &ids, bool processing,
    bool trash, String &message) const {
  cJSON *expected = cJSON_GetObjectItemCaseSensitive(
      asJson(jsonRoot), "expectedRevisions");
  if (!cJSON_IsObject(expected) || cJSON_GetArraySize(expected) != ids.size()) {
    message = "complete expectedRevisions are required";
    return false;
  }
  for (const String &id : ids) {
    const String directory = trash ? String(kCapsuleTrash) + "/" + id
                                   : activeCapsuleDirectory(id);
    const String metadata = directory +
        (trash ? "/trash.json" : (processing ? "/processing.json" : "/capsule.json"));
    cJSON *root = cJSON_Parse(readText(metadata, 8192).c_str());
    cJSON *revision = root == nullptr ? nullptr :
        cJSON_GetObjectItemCaseSensitive(root, "revision");
    cJSON *wanted = cJSON_GetObjectItemCaseSensitive(expected, id.c_str());
    const bool matches = !directory.isEmpty() && cJSON_IsNumber(revision) &&
        cJSON_IsNumber(wanted) && revision->valueint == wanted->valueint;
    cJSON_Delete(root);
    if (!matches) {
      message = "revision conflict for " + id;
      return false;
    }
  }
  return true;
}

bool PokePodLinkService::touchCapsule(const String &directory) {
  const String path = directory + "/capsule.json";
  cJSON *root = cJSON_Parse(readText(path, 8192).c_str());
  if (root == nullptr) return false;
  cJSON *revision = cJSON_GetObjectItemCaseSensitive(root, "revision");
  const int next = cJSON_IsNumber(revision) ? revision->valueint + 1 : 1;
  if (revision != nullptr) cJSON_SetNumberValue(revision, next);
  else cJSON_AddNumberToObject(root, "revision", next);
  replaceStringOrNull(root, "updatedAt", board_->utcNow().c_str());
  const bool ok = writeTextAtomic(path, printed(root) + "\n");
  cJSON_Delete(root);
  return ok;
}

bool PokePodLinkService::mutateFavoriteOrTags(
    void *jsonRoot, const char *operation, const std::vector<String> &ids,
    String &message) {
  if (ids.empty() || !validateExpectedRevisions(
          jsonRoot, ids, false, false, message)) return false;
  cJSON *root = asJson(jsonRoot);
  cJSON *requestedTags = cJSON_GetObjectItemCaseSensitive(root, "tags");
  cJSON *requestedFavorite = cJSON_GetObjectItemCaseSensitive(root, "favorite");
  std::vector<String> tags;
  if (strcmp(operation, "setFavorite") == 0) {
    if (!cJSON_IsBool(requestedFavorite)) {
      message = "favorite is required";
      return false;
    }
  } else {
    if (!cJSON_IsArray(requestedTags) || cJSON_GetArraySize(requestedTags) == 0) {
      message = "tags are required";
      return false;
    }
    cJSON *item = nullptr;
    cJSON_ArrayForEach(item, requestedTags) {
      const char *value = cJSON_GetStringValue(item);
      if (value == nullptr || value[0] == '#' || strlen(value) == 0 ||
          strlen(value) > 50) {
        message = "invalid tag";
        return false;
      }
      tags.emplace_back(value);
    }
  }
  struct Update { String path; String original; String value; };
  std::vector<Update> updates;
  for (const String &id : ids) {
    const String path = activeCapsuleDirectory(id) + "/capsule.json";
    const String original = readText(path, 8192);
    cJSON *capsule = cJSON_Parse(original.c_str());
    if (capsule == nullptr) {
      message = "capsule metadata is malformed";
      return false;
    }
    if (strcmp(operation, "setFavorite") == 0) {
      cJSON_ReplaceItemInObjectCaseSensitive(
          capsule, "favorite", cJSON_CreateBool(cJSON_IsTrue(requestedFavorite)));
    } else {
      cJSON *old = cJSON_GetObjectItemCaseSensitive(capsule, "tags");
      cJSON *replacement = cJSON_CreateArray();
      std::vector<String> values;
      cJSON *item = nullptr;
      cJSON_ArrayForEach(item, old) {
        const char *value = cJSON_GetStringValue(item);
        if (value != nullptr) values.emplace_back(value);
      }
      if (strcmp(operation, "addTags") == 0) {
        for (const String &tag : tags) {
          bool found = false;
          for (const String &value : values) {
            if (value.equalsIgnoreCase(tag)) found = true;
          }
          if (!found) values.push_back(tag);
        }
      } else if (strcmp(operation, "removeTags") == 0 ||
                 strcmp(operation, "deleteTag") == 0) {
        values.erase(std::remove_if(values.begin(), values.end(),
            [&](const String &value) {
              for (const String &tag : tags) if (value.equalsIgnoreCase(tag)) return true;
              return false;
            }), values.end());
      } else if (strcmp(operation, "renameTag") == 0 ||
                 strcmp(operation, "mergeTag") == 0) {
        if (tags.size() != 2) {
          cJSON_Delete(replacement);
          cJSON_Delete(capsule);
          message = "tag rename needs old and new names";
          return false;
        }
        for (String &value : values) {
          if (value.equalsIgnoreCase(tags[0])) value = tags[1];
        }
        for (size_t left = 0; left < values.size(); ++left) {
          values.erase(std::remove_if(values.begin() + left + 1, values.end(),
              [&](const String &value) { return value.equalsIgnoreCase(values[left]); }),
              values.end());
        }
      }
      for (const String &value : values) cJSON_AddItemToArray(replacement, cJSON_CreateString(value.c_str()));
      if (cJSON_HasObjectItem(capsule, "tags")) {
        cJSON_ReplaceItemInObjectCaseSensitive(capsule, "tags", replacement);
      } else {
        cJSON_AddItemToObject(capsule, "tags", replacement);
      }
    }
    cJSON *revision = cJSON_GetObjectItemCaseSensitive(capsule, "revision");
    if (!cJSON_IsNumber(revision)) {
      cJSON_Delete(capsule);
      message = "capsule revision is missing";
      return false;
    }
    cJSON_SetNumberValue(revision, revision->valueint + 1);
    replaceStringOrNull(capsule, "updatedAt", board_->utcNow().c_str());
    updates.push_back({path, original, printed(capsule) + "\n"});
    cJSON_Delete(capsule);
  }
  size_t committed = 0;
  for (; committed < updates.size(); ++committed) {
    if (!transferPermitted() ||
        !writeTextAtomic(updates[committed].path, updates[committed].value)) {
      const LinkBatchRollbackResult rollback = rollbackLinkBatch(
          committed, [&](size_t index) {
            (void)transferPermitted();
            const bool ok = writeTextAtomic(updates[index].path,
                                            updates[index].original);
            (void)transferPermitted();
            return ok;
          });
      message = rollback.ok() ? "capsule metadata commit failed"
                              : "capsule metadata rollback failed";
      return false;
    }
  }
  if (!transferPermitted()) {
    const LinkBatchRollbackResult rollback = rollbackLinkBatch(
        committed, [&](size_t index) {
          (void)transferPermitted();
          const bool ok = writeTextAtomic(updates[index].path,
                                          updates[index].original);
          (void)transferPermitted();
          return ok;
        });
    message = rollback.ok() ? "transfer deadline expired"
                            : "capsule metadata rollback failed";
    return false;
  }
  message = "committed";
  return true;
}

bool PokePodLinkService::moveOrCopy(void *jsonRoot, bool copy,
                                    const std::vector<String> &ids,
                                    String &message) {
  cJSON *root = asJson(jsonRoot);
  const char *destination = jsonString(root, "destination");
  const String targetFolder = folderDirectory(destination);
  if (ids.empty() || targetFolder.isEmpty() ||
      !storageExists(targetFolder, StorageAccess::read) ||
      !validateExpectedRevisions(root, ids, false, false, message)) {
    if (message.isEmpty()) message = "invalid move destination";
    return false;
  }
  struct PlannedTransfer {
    String id;
    String source;
    String target;
    String originalCapsule;
    bool noOp = false;
  };
  std::vector<PlannedTransfer> plans;
  for (const String &id : ids) {
    const String source = activeCapsuleDirectory(id);
    if (source.isEmpty()) {
      message = "capsule is missing";
      return false;
    }
    const String targetId = copy ? newUuid() : id;
    const String target = targetFolder + "/" + targetId;
    const bool noOp = !copy && source == target;
    for (const PlannedTransfer &plan : plans) {
      if (!noOp && plan.target == target) {
        message = "duplicate capsule target";
        return false;
      }
    }
    if (!noOp && storageExists(target, StorageAccess::read)) {
      message = "capsule target already exists";
      return false;
    }
    const String original = copy ? String()
        : readText(source + "/capsule.json", 8192);
    if (!copy && original.isEmpty()) {
      message = "capsule metadata is malformed";
      return false;
    }
    plans.push_back({targetId, source, target, original, noOp});
  }

  size_t applied = 0;
  bool failed = false;
  for (; applied < plans.size(); ++applied) {
    PlannedTransfer &plan = plans[applied];
    if (plan.noOp) continue;
    if (!transferPermitted()) {
      failed = true;
      break;
    }
    if (copy) {
      if (!copyTree(plan.source, plan.target) ||
          !transferPermitted() ||
          !rewriteCopiedMetadata(plan.target, plan.id)) {
        failed = true;
        break;
      }
    } else {
      if (!storageRename(plan.source, plan.target)) {
        failed = true;
        break;
      }
      if (!touchCapsule(plan.target)) {
        ++applied;
        failed = true;
        break;
      }
    }
  }
  if (!transferPermitted()) failed = true;
  if (failed || applied != plans.size()) {
    const bool deadlineExpired = !transferPermitted();
    const size_t rollbackCount = std::min(applied + (copy ? 1U : 0U),
                                          plans.size());
    const LinkBatchRollbackResult rollback = rollbackLinkBatch(
        rollbackCount, [&](size_t index) {
          (void)transferPermitted();
          PlannedTransfer &plan = plans[index];
          if (plan.noOp) {
            (void)transferPermitted();
            return true;
          }
          if (copy) {
            if (!storageExists(plan.target, StorageAccess::read)) {
              (void)transferPermitted();
              return true;
            }
            if (deadlineExpired || !deferredCommandFiles_.empty()) {
              const String staging = String(kCapsuleStaging) +
                  "/purge-copy-" + newUuid();
              if (storageRename(plan.target, staging)) {
                queueDeferredTreeCleanup(staging);
              } else {
                queueDeferredTreeCleanup(plan.target);
              }
              (void)transferPermitted();
              return true;
            }
            if (!removeTree(plan.target)) {
              queueDeferredTreeCleanup(plan.target);
            }
            (void)transferPermitted();
            return true;
          }
          if (!storageExists(plan.target, StorageAccess::read)) {
            (void)transferPermitted();
            return true;
          }
          const bool ok = storageRename(plan.target, plan.source) &&
              writeTextAtomic(plan.source + "/capsule.json",
                              plan.originalCapsule);
          (void)transferPermitted();
          return ok;
        });
    message = rollback.ok()
        ? (copy ? "capsule copy failed" : "capsule move failed")
        : (copy ? "capsule copy rollback failed"
                : "capsule move rollback failed");
    return false;
  }
  message = "committed";
  return true;
}

bool PokePodLinkService::trashOperation(void *jsonRoot, const char *operation,
                                        const std::vector<String> &ids,
                                        String &message) {
  cJSON *root = asJson(jsonRoot);
  const bool trash = strcmp(operation, "deleteCapsules") != 0;
  for (const String &id : ids) {
    const CapsuleSummary *record = library_->find(id);
    if (record != nullptr && record->readOnly) {
      message = "future schema capsule is read-only";
      return false;
    }
  }
  if (ids.empty() || !validateExpectedRevisions(
          root, ids, false, trash, message)) return false;
  if (strcmp(operation, "purgeCapsules") == 0) {
    if (!transferPermitted()) {
      message = "transfer deadline expired";
      return false;
    }
    const CapsuleBatchResult result = library_->purge(ids);
    if (!result.ok) {
      message = result.rollbackFailed > 0
          ? "purge rollback failed for " + result.rollbackFailedId
          : "purge failed for " + result.failedId;
      return false;
    }
    message = "committed";
    return true;
  }

  struct PlannedTrashMutation {
    String id;
    String source;
    String target;
    String originalCapsule;
    String originalTrash;
    String trashValue;
    bool metadataWritten = false;
    bool moved = false;
  };
  std::vector<PlannedTrashMutation> plans;
  for (const String &id : ids) {
    if (strcmp(operation, "deleteCapsules") == 0) {
      const String source = activeCapsuleDirectory(id);
      const String target = String(kCapsuleTrash) + "/" + id;
      const String originalCapsule = readText(source + "/capsule.json", 8192);
      if (source.isEmpty() || originalCapsule.isEmpty() ||
          storageExists(target, StorageAccess::read)) {
        message = "move to trash preflight failed";
        return false;
      }
      const int slash = source.lastIndexOf('/');
      String original = slash < 0 ? "Inbox" : source.substring(strlen(kCapsuleRoot) + 1, slash);
      if (original.isEmpty()) original = "Inbox";
      cJSON *capsule = cJSON_Parse(originalCapsule.c_str());
      cJSON *revision = capsule == nullptr ? nullptr : cJSON_GetObjectItemCaseSensitive(capsule, "revision");
      if (capsule == nullptr || !cJSON_IsNumber(revision)) {
        cJSON_Delete(capsule);
        message = "capsule metadata is malformed";
        return false;
      }
      const int nextRevision = cJSON_IsNumber(revision) ? revision->valueint + 1 : 2;
      cJSON_Delete(capsule);
      cJSON *metadata = cJSON_CreateObject();
      cJSON_AddNumberToObject(metadata, "schemaVersion", 1);
      cJSON_AddStringToObject(metadata, "capsuleId", id.c_str());
      cJSON_AddStringToObject(metadata, "trashedAt", board_->utcNow().c_str());
      cJSON_AddStringToObject(metadata, "originalFolder", original.c_str());
      cJSON_AddNumberToObject(metadata, "revision", nextRevision);
      const String trashValue = printed(metadata) + "\n";
      cJSON_Delete(metadata);
      plans.push_back({id, source, target, originalCapsule, String(),
                       trashValue, false, false});
    } else if (strcmp(operation, "restoreCapsules") == 0) {
      const String source = String(kCapsuleTrash) + "/" + id;
      const String originalTrash = readText(source + "/trash.json", 8192);
      const String originalCapsule = readText(source + "/capsule.json", 8192);
      cJSON *metadata = cJSON_Parse(originalTrash.c_str());
      const char *original = metadata == nullptr ? nullptr : jsonString(metadata, "originalFolder");
      String targetFolder = folderDirectory(original);
      if (targetFolder.isEmpty() ||
          !storageExists(targetFolder, StorageAccess::read)) {
        targetFolder = kCapsuleInbox;
      }
      const String target = targetFolder + "/" + id;
      cJSON_Delete(metadata);
      if (originalTrash.isEmpty() || originalCapsule.isEmpty() ||
          storageExists(target, StorageAccess::read)) {
        message = "trash restore preflight failed";
        return false;
      }
      plans.push_back({id, source, target, originalCapsule, originalTrash,
                       String(), false, false});
    }
  }

  bool failed = false;
  for (PlannedTrashMutation &plan : plans) {
    if (!transferPermitted()) {
      failed = true;
      break;
    }
    if (strcmp(operation, "deleteCapsules") == 0) {
      if (!writeTextAtomic(plan.source + "/trash.json", plan.trashValue)) {
        failed = true;
        break;
      }
      plan.metadataWritten = true;
      if (!storageRename(plan.source, plan.target)) {
        failed = true;
        break;
      }
      plan.moved = true;
    } else {
      if (!storageRename(plan.source, plan.target)) {
        failed = true;
        break;
      }
      plan.moved = true;
      if (!storageRemove(plan.target + "/trash.json") ||
          !touchCapsule(plan.target)) {
        failed = true;
        break;
      }
    }
  }
  if (!transferPermitted()) failed = true;
  if (failed) {
    const LinkBatchRollbackResult rollback = rollbackLinkBatch(
        plans.size(), [&](size_t index) {
      (void)transferPermitted();
      PlannedTrashMutation &item = plans[index];
      if (strcmp(operation, "deleteCapsules") == 0) {
        bool ok = true;
        if (item.moved && !storageRename(item.target, item.source)) ok = false;
        if (item.metadataWritten &&
            !storageRemove(item.source + "/trash.json")) ok = false;
        if (item.moved && !writeTextAtomic(
                item.source + "/capsule.json", item.originalCapsule)) {
          ok = false;
        }
        (void)transferPermitted();
        return ok;
      }
      if (!item.moved) {
        (void)transferPermitted();
        return true;
      }
      const bool ok = storageRename(item.target, item.source) &&
          writeTextAtomic(item.source + "/trash.json", item.originalTrash) &&
          writeTextAtomic(item.source + "/capsule.json",
                          item.originalCapsule);
      (void)transferPermitted();
      return ok;
    });
    message = rollback.ok()
        ? (strcmp(operation, "deleteCapsules") == 0
               ? "move to trash failed" : "trash restore failed")
        : "trash rollback failed";
    return false;
  }
  message = "committed";
  return true;
}

bool PokePodLinkService::folderOperation(void *jsonRoot,
                                         const char *operation,
                                         String &message) {
  cJSON *root = asJson(jsonRoot);
  const char *folder = jsonString(root, "folderPath");
  if (!safeFolder(folder, false)) {
    message = "invalid user folder";
    return false;
  }
  const String source = String(kCapsuleRoot) + "/" + folder;
  if (strcmp(operation, "createFolder") == 0) {
    if (storageExists(source, StorageAccess::read) ||
        !ensureDirectoryTree(source)) {
      message = "folder create failed";
      return false;
    }
  } else if (strcmp(operation, "renameFolder") == 0) {
    const char *newFolder = jsonString(root, "newFolderPath");
    if (!safeFolder(newFolder, false) ||
        !storageExists(source, StorageAccess::read)) {
      message = "invalid folder rename";
      return false;
    }
    const String target = String(kCapsuleRoot) + "/" + newFolder;
    if (storageExists(target, StorageAccess::read) ||
        !ensureDirectoryTree(parentPath(target)) ||
        !storageRename(source, target)) {
      message = "folder rename failed";
      return false;
    }
  } else if (strcmp(operation, "deleteFolderToInbox") == 0) {
    std::vector<String> files;
    if (!collectFiles(source, "", 0, files)) {
      message = transferPermitted() ? "folder enumeration failed"
                                    : "transfer deadline expired";
      return false;
    }
    struct PlannedEvacuation {
      String id;
      String source;
      String target;
      String originalCapsule;
      bool moved = false;
    };
    std::vector<PlannedEvacuation> plans;
    for (const String &file : files) {
      if (!file.endsWith("/capsule.json") && file != "capsule.json") continue;
      const String relativeDirectory = file.substring(0, file.length() - strlen("/capsule.json"));
      const int slash = relativeDirectory.lastIndexOf('/');
      const String id = slash < 0 ? relativeDirectory : relativeDirectory.substring(slash + 1);
      if (!isUuid(id.c_str())) {
        message = "folder contains malformed capsule directory";
        return false;
      }
      const String capsuleSource = source + "/" + relativeDirectory;
      const String target = String(kCapsuleInbox) + "/" + id;
      const String originalCapsule = readText(
          capsuleSource + "/capsule.json", 8192);
      if (originalCapsule.isEmpty() ||
          storageExists(target, StorageAccess::read)) {
        message = "folder evacuation preflight failed";
        return false;
      }
      plans.push_back({id, capsuleSource, target, originalCapsule, false});
    }

    std::vector<String> expectedIds;
    if (!collectCommandIds(root, expectedIds) ||
        expectedIds.size() != plans.size()) {
      message = "folder contents changed during preflight";
      return false;
    }
    for (const String &expected : expectedIds) {
      bool found = false;
      for (const PlannedEvacuation &plan : plans) {
        if (plan.id.equalsIgnoreCase(expected)) found = true;
      }
      if (!found) {
        message = "folder contents changed during preflight";
        return false;
      }
    }

    bool failed = false;
    for (PlannedEvacuation &plan : plans) {
      if (!transferPermitted() ||
          !storageRename(plan.source, plan.target)) {
        failed = true;
        break;
      }
      plan.moved = true;
      if (!touchCapsule(plan.target)) {
        failed = true;
        break;
      }
    }
    const String staging = String(kCapsuleStaging) + "/purge-folder-" +
        newUuid();
    if (!failed && (!transferPermitted() ||
                    !storageRename(source, staging))) {
      failed = true;
    }
    if (failed) {
      const LinkBatchRollbackResult rollback = rollbackLinkBatch(
          plans.size(), [&](size_t index) {
            (void)transferPermitted();
            PlannedEvacuation &item = plans[index];
            const bool ok = !item.moved ||
                (storageRename(item.target, item.source) &&
                 writeTextAtomic(item.source + "/capsule.json",
                                 item.originalCapsule));
            (void)transferPermitted();
            return ok;
          });
      message = rollback.ok() ? "folder evacuation failed"
                              : "folder evacuation rollback failed";
      return false;
    }
    queueDeferredTreeCleanup(staging);
    log_->printf("{\"event\":\"folder_cleanup_deferred\",\"path\":\"%s\"}\n",
                 staging.c_str());
  }
  message = "committed";
  return true;
}

bool PokePodLinkService::cleanupPurgeStaging() {
  if (fs_ == nullptr) return false;
  StorageIoLease lease = StorageCoordinator::instance().acquireIo(
      storageOwner(), StorageAccess::read, storageIoTimeout());
  if (!lease) return false;
  File root = fs_->open(kCapsuleStaging);
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    return true;
  }
  while (true) {
    File entry = root.openNextFile();
    if (!entry) break;
    const String full = entry.name();
    const bool isDirectory = entry.isDirectory();
    entry.close();
    const int slash = full.lastIndexOf('/');
    const String name = slash >= 0 ? full.substring(slash + 1) : full;
    if (isDirectory && purgeStagingDirectoryName(name.c_str())) {
      queueDeferredTreeCleanup(String(kCapsuleStaging) + "/" + name);
    }
  }
  root.close();
  return true;
}

void PokePodLinkService::queueDeferredTreeCleanup(const String &path) {
  if (path.isEmpty()) return;
  for (const String &queued : deferredTreeCleanupStack_) {
    if (queued == path) return;
  }
  deferredTreeCleanupStack_.push_back(path);
}

bool PokePodLinkService::stepDeferredTreeCleanup() {
  if (deferredTreeCleanupStack_.empty() || fs_ == nullptr) return true;
  StorageIoLease lease = StorageCoordinator::instance().acquireIo(
      commandStorageActive_ ? StorageOwner::capsuleTransaction
                            : StorageOwner::recovery,
      StorageAccess::mutation, 0);
  if (!lease) return false;
  const String path = deferredTreeCleanupStack_.back();
  File root = fs_->open(path);
  if (!root) {
    deferredTreeCleanupStack_.pop_back();
    return deferredTreeCleanupStack_.empty();
  }
  if (!root.isDirectory()) {
    root.close();
    if (fs_->remove(path)) deferredTreeCleanupStack_.pop_back();
    return deferredTreeCleanupStack_.empty();
  }
  File entry = root.openNextFile();
  if (!entry) {
    root.close();
    if (fs_->rmdir(path)) deferredTreeCleanupStack_.pop_back();
    return deferredTreeCleanupStack_.empty();
  }
  const String full = entry.name();
  entry.close();
  root.close();
  const int slash = full.lastIndexOf('/');
  const String name = slash >= 0 ? full.substring(slash + 1) : full;
  deferredTreeCleanupStack_.push_back(path + "/" + name);
  return false;
}

bool PokePodLinkService::removeTree(const String &path) {
  File root;
  bool directory = false;
  {
    StorageIoLease lease = StorageCoordinator::instance().acquireIo(
        storageOwner(), StorageAccess::mutation, storageIoTimeout());
    if (!lease) return false;
    root = fs_->open(path);
    if (!root) return true;
    directory = root.isDirectory();
    if (!directory) root.close();
  }
  if (!directory) return storageRemove(path);
  std::vector<String> children;
  while (true) {
    String full;
    {
      StorageIoLease lease = StorageCoordinator::instance().acquireIo(
          storageOwner(), StorageAccess::mutation, storageIoTimeout());
      if (!lease) {
        closeStorageFile(root, StorageAccess::mutation);
        return false;
      }
      File entry = root.openNextFile();
      if (!entry) break;
      full = entry.name();
      entry.close();
    }
    const int slash = full.lastIndexOf('/');
    const String name = slash >= 0 ? full.substring(slash + 1) : full;
    children.push_back(path + "/" + name);
  }
  closeStorageFile(root, StorageAccess::mutation);
  for (const String &child : children) {
    if (!removeTree(child)) return false;
  }
  return storageRmdir(path);
}

bool PokePodLinkService::copyTree(const String &source, const String &target,
                                  uint8_t depth) {
  if (!transferPermitted() || depth > 6) return false;
  File input;
  bool directory = false;
  {
    StorageIoLease lease = StorageCoordinator::instance().acquireIo(
        storageOwner(), StorageAccess::read, storageIoTimeout());
    if (!lease) return false;
    input = fs_->open(source, FILE_READ);
    if (!input) return false;
    directory = input.isDirectory();
  }
  if (!directory) {
    File output;
    {
      StorageIoLease lease = StorageCoordinator::instance().acquireIo(
          storageOwner(), StorageAccess::mutation, storageIoTimeout());
      if (!lease) {
        closeStorageFile(input, StorageAccess::read);
        return false;
      }
      output = fs_->open(target, FILE_WRITE);
      if (!output) {
        input.close();
        return false;
      }
    }
    uint8_t buffer[4096];
    bool ok = true;
    while (transferPermitted()) {
      size_t count = 0;
      bool available = false;
      {
        StorageIoLease lease = StorageCoordinator::instance().acquireIo(
            storageOwner(), StorageAccess::mutation, storageIoTimeout());
        if (!lease) {
          ok = false;
          break;
        }
        available = input.available();
        if (!available) break;
        count = input.read(buffer, sizeof(buffer));
        if (count == 0 || output.write(buffer, count) != count) ok = false;
      }
      if (!ok) {
        ok = false;
        break;
      }
    }
    if (!transferPermitted()) ok = false;
    const bool filesFinalized = finishCopiedFiles(input, output);
    return ok && filesFinalized && transferPermitted();
  }
  closeStorageFile(input, StorageAccess::read);
  if (!storageExists(target, StorageAccess::read) && !storageMkdir(target)) {
    return false;
  }
  File sourceDirectory;
  {
    StorageIoLease lease = StorageCoordinator::instance().acquireIo(
        storageOwner(), StorageAccess::read, storageIoTimeout());
    if (!lease) return false;
    sourceDirectory = fs_->open(source);
    if (!sourceDirectory || !sourceDirectory.isDirectory()) {
      if (sourceDirectory) sourceDirectory.close();
      return false;
    }
  }
  while (transferPermitted()) {
    String full;
    {
      StorageIoLease lease = StorageCoordinator::instance().acquireIo(
          storageOwner(), StorageAccess::read, storageIoTimeout());
      if (!lease) {
        closeStorageFile(sourceDirectory, StorageAccess::read);
        return false;
      }
      File entry = sourceDirectory.openNextFile();
      if (!entry) break;
      full = entry.name();
      entry.close();
    }
    const int slash = full.lastIndexOf('/');
    const String name = slash < 0 ? full : full.substring(slash + 1);
    if (!copyTree(source + "/" + name, target + "/" + name, depth + 1)) {
      closeStorageFile(sourceDirectory, StorageAccess::read);
      return false;
    }
  }
  closeStorageFile(sourceDirectory, StorageAccess::read);
  return transferPermitted();
}

bool PokePodLinkService::rewriteCopiedMetadata(const String &directory,
                                               const String &id) {
  const String capsulePath = directory + "/capsule.json";
  const String processingPath = directory + "/processing.json";
  cJSON *capsule = cJSON_Parse(readText(capsulePath, 8192).c_str());
  cJSON *processing = cJSON_Parse(readText(processingPath, 8192).c_str());
  if (capsule == nullptr || processing == nullptr) {
    cJSON_Delete(capsule);
    cJSON_Delete(processing);
    return false;
  }
  replaceStringOrNull(capsule, "id", id.c_str());
  replaceStringOrNull(capsule, "createdAt", board_->utcNow().c_str());
  replaceStringOrNull(capsule, "updatedAt", board_->utcNow().c_str());
  cJSON_ReplaceItemInObjectCaseSensitive(capsule, "revision", cJSON_CreateNumber(1));
  replaceStringOrNull(processing, "capsuleId", id.c_str());
  cJSON *processingRevision = cJSON_GetObjectItemCaseSensitive(processing, "revision");
  if (!cJSON_IsNumber(processingRevision)) {
    cJSON_Delete(capsule);
    cJSON_Delete(processing);
    return false;
  }
  cJSON_SetNumberValue(processingRevision, processingRevision->valueint + 1);
  const String capsuleValue = printed(capsule) + "\n";
  const String processingValue = printed(processing) + "\n";
  const bool ok = transaction_.commitTextPair(
      id.c_str(), capsulePath, capsuleValue, processingPath, processingValue,
      storageOwner());
  cJSON_Delete(capsule);
  cJSON_Delete(processing);
  return ok;
}

String PokePodLinkService::newUuid() const {
  uint8_t bytes[16];
  for (size_t offset = 0; offset < sizeof(bytes); offset += 4) {
    const uint32_t value = esp_random();
    memcpy(bytes + offset, &value, sizeof(value));
  }
  char result[37];
  formatUuidV4(bytes, result);
  return String(result);
}

String PokePodLinkService::provisioningDiagnosticsJson() const {
  cJSON *root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "status", "ok");
  cJSON_AddNumberToObject(root, "version", kLinkVersion);
  cJSON_AddNumberToObject(root, "schemaVersion", 1);
  cJSON *records = cJSON_AddArrayToObject(root, "records");
  if (provisioningDiagnostics_ != nullptr) {
    for (size_t index = 0; index < provisioningDiagnostics_->count(); ++index) {
      const StoredProvisioningLogRecord *record =
          provisioningDiagnostics_->newest(index);
      if (record == nullptr) continue;
      cJSON *item = cJSON_CreateObject();
      cJSON_AddNumberToObject(item, "sequence", record->sequence);
      cJSON_AddNumberToObject(item, "epoch", record->epoch);
      cJSON_AddNumberToObject(item, "elapsedMs", record->elapsedMs);
      cJSON_AddStringToObject(item, "stage", provisioningLogStageKey(
          static_cast<ProvisioningLogStage>(record->stage)));
      cJSON_AddNumberToObject(item, "outcome", record->outcome);
      cJSON_AddNumberToObject(item, "attempt", record->attempt);
      cJSON_AddStringToObject(item, "ssid", record->ssid);
      cJSON_AddNumberToObject(item, "rssi", record->rssi);
      cJSON_AddNumberToObject(item, "reason", record->reason);
      cJSON_AddStringToObject(item, "reasonKind",
                              wifiFailureKey(record->reason));
      cJSON_AddItemToArray(records, item);
    }
  }
  const String result = printed(root);
  cJSON_Delete(root);
  return result;
}

String PokePodLinkService::powerDiagnosticsJson() const {
  cJSON *root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "status", "ok");
  cJSON_AddNumberToObject(root, "version", kLinkVersion);
  cJSON_AddNumberToObject(root, "schemaVersion", 1);
  cJSON *blockerKeys = cJSON_AddArrayToObject(root, "blockerKeys");
  for (uint8_t bit = 0;
       bit <= static_cast<uint8_t>(PowerBlocker::beforeDeepTimeout); ++bit) {
    cJSON_AddItemToArray(blockerKeys, cJSON_CreateString(powerBlockerKey(
        static_cast<PowerBlocker>(bit))));
  }
  cJSON *records = cJSON_AddArrayToObject(root, "records");
  if (powerDiagnostics_ != nullptr) {
    for (size_t index = 0; index < powerDiagnostics_->count(); ++index) {
      const StoredPowerLogRecord *record = powerDiagnostics_->newest(index);
      if (record == nullptr) continue;
      cJSON *item = cJSON_CreateObject();
      cJSON_AddNumberToObject(item, "sequence", record->sequence);
      cJSON_AddNumberToObject(item, "epoch", record->epoch);
      cJSON_AddNumberToObject(item, "uptimeMs", record->uptimeMs);
      cJSON_AddNumberToObject(item, "durationMs", record->durationMs);
      cJSON_AddStringToObject(item, "event", powerLogEventKey(
          static_cast<PowerLogEvent>(record->event)));
      cJSON_AddStringToObject(item, "mode", powerModeName(
          static_cast<PowerMode>(record->mode)));
      cJSON_AddNumberToObject(item, "blockerMask", record->blockers);
      cJSON_AddNumberToObject(item, "detail", record->detail);
      char wakeMask[19];
      snprintf(wakeMask, sizeof(wakeMask), "%016llx",
               static_cast<unsigned long long>(record->ext1WakeMask));
      cJSON_AddStringToObject(item, "wakeMask", wakeMask);
      cJSON_AddNumberToObject(item, "error", record->error);
      cJSON_AddNumberToObject(item, "flags", record->flags);
      cJSON_AddNumberToObject(item, "resetReason", record->resetReason);
      cJSON_AddNumberToObject(item, "wakeCause", record->wakeCause);
      cJSON_AddNumberToObject(item, "batteryPercent",
                              record->batteryPercent);
      cJSON_AddItemToArray(records, item);
    }
  }
  const String result = printed(root);
  cJSON_Delete(root);
  return result;
}

bool PokePodLinkService::executeCommand(const String &path,
                                        const String &transactionId,
                                        String &message) {
  const String source = readText(path, kMaxCommandJsonBytes);
  cJSON *root = cJSON_ParseWithLength(source.c_str(), source.length());
  bool success = false;
  bool completedMaintenance = false;
  if (root == nullptr || !cJSON_IsObject(root)) {
    message = "command JSON is malformed";
  } else if (jsonInt64(root, "schemaVersion") != 2 ||
             !sameUuid(jsonString(root, "transactionId"), transactionId.c_str())) {
    message = "command schema or transactionId is invalid";
  } else {
    const char *operation = jsonString(root, "operation");
    if (operation == nullptr) {
      message = "command operation is missing";
    } else if (strcmp(operation, "rescan") == 0) {
      success = library_->requestScan();
      message = success ? "queued" : "rescan request failed";
    } else {
      // The maintenance lifecycle is persisted in the command result directory.
      // Mutating commands are implemented by the shared capsule library below;
      // unsupported high-level organization commands fail explicitly and never
      // partially mutate the SD card.
      const char *maintenanceId = jsonString(root, "maintenanceId");
      if (strcmp(operation, "beginMaintenance") == 0) {
        if (!isUuid(maintenanceId)) message = "invalid maintenanceId";
        else if (!activeMaintenance_.isEmpty() && !activeMaintenance_.equalsIgnoreCase(maintenanceId)) {
          message = "another maintenance session is active";
        } else {
          activeMaintenance_ = maintenanceId;
          // A newly accepted transaction immediately invalidates the prior
          // session's completed presentation, even if persisting this command
          // result later fails.
          maintenanceCompletion_.beginAccepted();
          success = true;
          message = "committed";
        }
      } else if (strcmp(operation, "endMaintenance") == 0) {
        if (activeMaintenance_.isEmpty() || !activeMaintenance_.equalsIgnoreCase(maintenanceId)) {
          message = "maintenance session does not own the device";
        } else {
          activeMaintenance_ = "";
          success = true;
          completedMaintenance = true;
          message = "committed";
        }
      } else if (activeMaintenance_.isEmpty() ||
                 !activeMaintenance_.equalsIgnoreCase(maintenanceId)) {
        message = "maintenance session does not own the device";
      } else {
        std::vector<String> commandIds;
        const bool hasIds = collectCommandIds(root, commandIds);
        const char *id = hasIds && commandIds.size() == 1
            ? commandIds[0].c_str() : nullptr;
        if (strcmp(operation, "createFolder") == 0 ||
            strcmp(operation, "renameFolder") == 0) {
          success = folderOperation(root, operation, message);
        } else if (strcmp(operation, "deleteFolderToInbox") == 0) {
          if (!hasIds || !validateExpectedRevisions(
                  root, commandIds, false, false, message)) {
            success = false;
          } else success = folderOperation(root, operation, message);
        } else if ((strcmp(operation, "moveCapsules") == 0 ||
                    strcmp(operation, "copyCapsules") == 0) && hasIds) {
          success = moveOrCopy(root, strcmp(operation, "copyCapsules") == 0,
                               commandIds, message);
        } else if ((strcmp(operation, "deleteCapsules") == 0 ||
                    strcmp(operation, "restoreCapsules") == 0 ||
                    strcmp(operation, "purgeCapsules") == 0) && hasIds) {
          success = trashOperation(root, operation, commandIds, message);
        } else if ((strcmp(operation, "setFavorite") == 0 ||
                    strcmp(operation, "addTags") == 0 ||
                    strcmp(operation, "removeTags") == 0 ||
                    strcmp(operation, "renameTag") == 0 ||
                    strcmp(operation, "mergeTag") == 0 ||
                    strcmp(operation, "deleteTag") == 0) && hasIds) {
          if (commandIds.empty() && (strcmp(operation, "renameTag") == 0 ||
                                     strcmp(operation, "mergeTag") == 0 ||
                                     strcmp(operation, "deleteTag") == 0)) {
            success = true;
            message = "committed";
          } else {
            success = mutateFavoriteOrTags(root, operation, commandIds, message);
          }
        } else if (strcmp(operation, "requeueTranscription") == 0 && isUuid(id)) {
          if (!validateExpectedRevisions(root, commandIds, true, false, message)) {
            success = false;
          } else {
            const CapsuleSummary *record = library_->find(id);
            if (record == nullptr || record->status != CapsuleStatus::failed) {
              message = "only failed capsules can be requeued";
            } else {
              success = library_->requeue(id);
              if (success) tencent_->wake();
              message = success ? "committed" : "requeue failed";
            }
          }
        } else if (strcmp(operation, "commitCorrection") == 0 && isUuid(id)) {
          const CapsuleSummary *record = library_->find(id);
          const char *stagedPath = jsonString(root, "stagedPath");
          const String expectedStaged = ".staging/" + transactionId + "/" + id;
          const String staged = String(kCapsuleRoot) + "/" +
              (stagedPath == nullptr ? "" : stagedPath) + "/polished.md";
          const String text = readText(staged, 1024 * 1024);
          if (record == nullptr || stagedPath == nullptr ||
              String(stagedPath) != expectedStaged || text.isEmpty()) {
            message = "invalid staged correction";
          } else {
            const String processingPath = record->directory + "/processing.json";
            const String polishedPath = record->directory + "/polished.md";
            const String processingText = readText(processingPath, 8192);
            cJSON *processing = cJSON_Parse(processingText.c_str());
            if (processing == nullptr) message = "processing metadata is malformed";
            else {
              cJSON *revision = cJSON_GetObjectItemCaseSensitive(processing, "revision");
              const int expected = static_cast<int>(jsonInt64(root, "expectedRevision"));
              if (!cJSON_IsNumber(revision) || revision->valueint != expected) {
                message = "processing revision conflict";
              } else {
                cJSON_SetNumberValue(revision, expected + 1);
                cJSON_ReplaceItemInObjectCaseSensitive(processing, "status", cJSON_CreateString("ready"));
                replaceStringOrNull(processing, "polishedTextFile", "polished.md");
                replaceStringOrNull(processing, "errorStage", nullptr);
                replaceStringOrNull(processing, "error", nullptr);
                const String processingValue = printed(processing) + "\n";
                success = transaction_.commitTextPair(
                    transactionId.c_str(), polishedPath, text,
                    processingPath, processingValue, storageOwner());
                message = success ? "committed" : "correction commit failed";
                if (success) {
                  const String stagedDirectory = String(kCapsuleRoot) + "/" + expectedStaged;
                  queueDeferredTreeCleanup(stagedDirectory);
                }
              }
              cJSON_Delete(processing);
            }
          }
        } else if (strcmp(operation, "commitFinalText") == 0 && isUuid(id)) {
          const CapsuleSummary *record = library_->find(id);
          const char *inlineText = jsonString(root, "finalText");
          String text = inlineText == nullptr ? String() : String(inlineText);
          const char *stagedPath = jsonString(root, "stagedPath");
          const String expectedStaged = ".staging/" + transactionId + "/" + id;
          const String stagedFinal = String(kCapsuleRoot) + "/" +
              expectedStaged + "/final.md";
          const bool stagedFinalExists = storageExists(
              stagedFinal, StorageAccess::read);
          if (inlineText == nullptr && stagedPath != nullptr &&
              String(stagedPath) == expectedStaged) {
            text = readText(stagedFinal, 1024 * 1024);
          }
          if (record == nullptr || (inlineText == nullptr && !stagedFinalExists)) {
            message = "invalid final text command";
          } else {
            const String capsulePath = record->directory + "/capsule.json";
            const String finalPath = record->directory + "/final.md";
            cJSON *capsule = cJSON_Parse(readText(capsulePath, 8192).c_str());
            cJSON *revision = capsule == nullptr ? nullptr :
                cJSON_GetObjectItemCaseSensitive(capsule, "revision");
            const int expected = static_cast<int>(jsonInt64(root, "expectedRevision"));
            if (!cJSON_IsNumber(revision) || revision->valueint != expected) {
              message = "capsule revision conflict";
            } else {
              cJSON_SetNumberValue(revision, expected + 1);
              replaceStringOrNull(capsule, "updatedAt", board_->utcNow().c_str());
              const String capsuleValue = printed(capsule) + "\n";
              success = transaction_.commitTextPair(
                  transactionId.c_str(), finalPath, text,
                  capsulePath, capsuleValue, storageOwner());
              message = success ? "committed" : "final text commit failed";
              if (success && stagedPath != nullptr) {
                const String stagedDirectory = String(kCapsuleRoot) + "/" + expectedStaged;
                queueDeferredTreeCleanup(stagedDirectory);
              }
            }
            cJSON_Delete(capsule);
          }
        } else if (strcmp(operation, "commitImport") == 0 && isUuid(id)) {
          const char *stagedPath = jsonString(root, "stagedPath");
          const char *destination = jsonString(root, "destination");
          const String staged = String(kCapsuleRoot) + "/" +
              (stagedPath == nullptr ? "" : stagedPath);
          const String targetFolder = folderDirectory(destination);
          const String target = targetFolder + "/" + id;
          const String capsuleText = readText(staged + "/capsule.json", 8192);
          const String processingText = readText(staged + "/processing.json", 8192);
          cJSON *capsule = cJSON_Parse(capsuleText.c_str());
          cJSON *processing = cJSON_Parse(processingText.c_str());
          const char *metadataId = capsule == nullptr ? nullptr : jsonString(capsule, "id");
          const char *processingId = processing == nullptr ? nullptr :
              jsonString(processing, "capsuleId");
          const char *audioFile = processing == nullptr ? nullptr :
              jsonString(processing, "audioFile");
          const int64_t processingSchema = processing == nullptr ? -1 :
              jsonInt64(processing, "schemaVersion");
          const String expectedStaged = ".staging/" + transactionId + "/" + id;
          if (stagedPath == nullptr || String(stagedPath) != expectedStaged ||
              targetFolder.isEmpty() || !sameUuid(id, metadataId) ||
              !sameUuid(id, processingId) ||
              (processingSchema != 1 && processingSchema != 2) ||
              !safeCapsuleFileName(audioFile) ||
              !storageExists(staged + "/" + audioFile, StorageAccess::read) ||
              !storageExists(targetFolder, StorageAccess::read) ||
              storageExists(target, StorageAccess::read)) {
            message = "invalid staged import";
          } else {
            success = storageRename(staged, target);
            message = success ? "committed" : "import commit failed";
            if (success) storageRmdir(parentPath(staged));
          }
          cJSON_Delete(capsule);
          cJSON_Delete(processing);
        } else {
          message = "command operation is not supported by this firmware";
        }
      }
    }
  }

  cJSON *result = cJSON_CreateObject();
  cJSON_AddNumberToObject(result, "schemaVersion", 1);
  cJSON_AddStringToObject(result, "commandId", transactionId.c_str());
  cJSON_AddStringToObject(result, "transactionId", transactionId.c_str());
  cJSON_AddBoolToObject(result, "ok", success);
  cJSON_AddBoolToObject(result, "success", success);
  cJSON_AddStringToObject(result, "message", message.c_str());
  cJSON_AddStringToObject(result, "completedAt", board_->utcNow().c_str());
  const String resultPath = String(kCapsuleSystem) + "/commands/results/" +
      transactionId + ".json";
  const bool persisted = writeTextAtomic(resultPath, printed(result) + "\n");
  cJSON_Delete(result);
  cJSON_Delete(root);
  // executeCommand() runs under the Link mutation reservation. Queueing is
  // intentionally accepted here; pollScan() starts only after that owner has
  // released the reservation.
  (void)library_->requestScan();
  if (persisted && completedMaintenance) {
    maintenanceCompletion_.endResultPersisted(transactionId.c_str());
  }
  return persisted;
}

bool PokePodLinkService::sendOk(uint32_t requestId, const char *extraJson) {
  String value = "{\"status\":\"ok\",\"version\":2";
  if (extraJson != nullptr && extraJson[0] != '\0') {
    value += ',';
    value += extraJson;
  }
  value += '}';
  return sendJson(requestId, value);
}

void PokePodLinkService::sendBusy(uint32_t requestId, uint32_t retryAfterMs) {
  sendJson(requestId, "{\"status\":\"busy\",\"version\":2,\"retryAfterMs\":" +
      String(retryAfterMs) + "}");
}

void PokePodLinkService::sendError(uint32_t requestId, const char *message) {
  cJSON *root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "status", "error");
  cJSON_AddNumberToObject(root, "version", 2);
  cJSON_AddStringToObject(root, "message", message == nullptr ? "error" : message);
  sendJson(requestId, printed(root));
  cJSON_Delete(root);
}

bool PokePodLinkService::sendJson(uint32_t requestId, const String &json) {
  if (json.length() > kLinkMaxControlBytes) return false;
  return sendFrame(LinkFrameType::responseJson, 0, requestId,
                   reinterpret_cast<const uint8_t *>(json.c_str()), json.length());
}

bool PokePodLinkService::sendEvent(uint32_t requestId, const String &json) {
  if (json.length() > kLinkMaxControlBytes) return false;
  return sendFrame(LinkFrameType::eventJson, 0, requestId,
                   reinterpret_cast<const uint8_t *>(json.c_str()), json.length());
}

bool PokePodLinkService::sendFile(uint32_t requestId, const String &path,
                                  const char *resultTransactionId) {
  if (!transferPermitted()) return false;
  if (outgoingPhase_ != OutgoingPhase::none || txStepper_.active() ||
      pendingControlBytes_ != 0) {
    sendBusy(requestId);
    return false;
  }
  StorageReservation reservation = StorageCoordinator::instance().reserve(
      storageOwner(), StorageAccess::read, 250);
  if (!reservation) {
    sendBusy(requestId);
    return false;
  }
  File file;
  size_t length = 0;
  {
    StorageIoLease lease = StorageCoordinator::instance().acquireIo(
        storageOwner(), StorageAccess::read, storageIoTimeout());
    if (!lease) return false;
    file = fs_->open(path, FILE_READ);
    if (!file || file.isDirectory()) {
      if (file) file.close();
      sendError(requestId, "file is missing");
      return false;
    }
    length = file.size();
  }
  const String response =
      "{\"status\":\"ok\",\"version\":2,\"available\":true,"
      "\"binaryLength\":" + String(length) + "}";
  outgoingPhase_ = OutgoingPhase::response;
  outgoingRequestId_ = requestId;
  outgoingLength_ = length;
  outgoingRead_ = 0;
  outgoingFile_ = file;
  outgoingResultTransactionId_ = resultTransactionId == nullptr
      ? String() : String(resultTransactionId);
  outgoingStorageReservation_ = std::move(reservation);
  if (!queueFrame(LinkFrameType::responseJson, 0, requestId,
                  reinterpret_cast<const uint8_t *>(response.c_str()),
                  response.length(), TxCompletion::fileResponse)) {
    finishOutgoingFile(false);
    return false;
  }
  return true;
}

bool PokePodLinkService::sendFrame(LinkFrameType type, uint16_t flags,
                                   uint32_t requestId,
                                   const uint8_t *payload, size_t size) {
  return queueFrame(type, flags, requestId, payload, size,
                    TxCompletion::none);
}

bool PokePodLinkService::queueFrame(LinkFrameType type, uint16_t flags,
                                    uint32_t requestId,
                                    const uint8_t *payload, size_t size,
                                    TxCompletion completion) {
  if (stream_ == nullptr || !transferPermitted() || size >
      (type == LinkFrameType::data ? kLinkMaxDataBytes : kLinkMaxControlBytes)) {
    return false;
  }
  LinkFrameHeader header;
  header.type = type;
  header.flags = flags;
  header.requestId = requestId;
  header.payloadLength = size;
  header.payloadCrc32 = linkCrc32(payload, size);
  uint8_t *target = nullptr;
  size_t *targetBytes = nullptr;
  TxCompletion *targetCompletion = nullptr;
  if (!txStepper_.active() && txFrameBytes_ == 0) {
    target = txFrame_;
    targetBytes = &txFrameBytes_;
    targetCompletion = &txCompletion_;
  } else if (type != LinkFrameType::data && pendingControlBytes_ == 0) {
    target = pendingControlFrame_;
    targetBytes = &pendingControlBytes_;
    targetCompletion = &pendingControlCompletion_;
  } else {
    return false;
  }
  if (target == nullptr ||
      !encodeLinkHeader(header, target, kLinkHeaderBytes)) return false;
  if (size > 0) memcpy(target + kLinkHeaderBytes, payload, size);
  *targetBytes = kLinkHeaderBytes + size;
  *targetCompletion = completion;
  if (target == txFrame_ && !txStepper_.beginFrame(*targetBytes)) {
    *targetBytes = 0;
    *targetCompletion = TxCompletion::none;
    return false;
  }
  if (requestLeaseHeld_ && activeMaintenance_.isEmpty()) {
    releaseRequestLeaseWhenTxDrained_ = true;
  }
  return true;
}

void PokePodLinkService::advanceTransmit(uint32_t nowMs) {
  if (!txStepper_.active() || txFrame_ == nullptr) return;
  const bool permitted = linkTransferPermitted(transferGate_, nowMs);
  const size_t wanted = txStepper_.nextWriteBytes(
      permitted, kLinkWriteSliceBytes);
  if (wanted == 0) {
    if (!permitted) disconnect();
    return;
  }
  const uint8_t *source = txFrame_ + txStepper_.offset();
  LinkWriteAttempt attempt;
  if (writeChannel_ != nullptr) {
    attempt = writeChannel_->writeSome(source, wanted);
  } else {
    const int available = stream_ == nullptr ? 0 :
        stream_->availableForWrite();
    const size_t writable = available <= 0 ? 0 :
        std::min<size_t>(wanted, static_cast<size_t>(available));
    const size_t written = writable == 0 ? 0 :
        stream_->write(source, writable);
    attempt.disposition = written == 0 ? LinkWriteDisposition::wouldBlock :
                                         LinkWriteDisposition::progress;
    attempt.bytes = written;
  }
  const bool permittedAfter = transferPermitted();
  const LinkTransferStepResult result =
      txStepper_.accept(permittedAfter, attempt);
  if (result == LinkTransferStepResult::cancelled ||
      result == LinkTransferStepResult::disconnected ||
      result == LinkTransferStepResult::failed) {
    disconnect();
    return;
  }
  if (result != LinkTransferStepResult::frameComplete) return;

  const TxCompletion completion = txCompletion_;
  txFrameBytes_ = 0;
  txCompletion_ = TxCompletion::none;
  onFrameSent(completion);

  if (!txStepper_.active() && pendingControlBytes_ > 0) {
    memcpy(txFrame_, pendingControlFrame_, pendingControlBytes_);
    txFrameBytes_ = pendingControlBytes_;
    txCompletion_ = pendingControlCompletion_;
    pendingControlBytes_ = 0;
    pendingControlCompletion_ = TxCompletion::none;
    txStepper_.beginFrame(txFrameBytes_);
  }
  if (!txStepper_.active() && pendingControlBytes_ == 0 &&
      outgoingPhase_ == OutgoingPhase::none &&
      releaseRequestLeaseWhenTxDrained_) {
    releaseRequestLeaseNow();
  }
}

void PokePodLinkService::queueNextFileChunk() {
  if (outgoingPhase_ != OutgoingPhase::data || txStepper_.active() ||
      !outgoingFile_) return;
  if (!transferPermitted()) {
    disconnect();
    return;
  }
  if (outgoingRead_ >= outgoingLength_) {
    finishOutgoingFile(true);
    return;
  }
  size_t count = 0;
  {
    // Do not wait behind another SD user while the Link state owns no physical
    // IO lease. A later poll retries without holding the network path hostage.
    StorageIoLease lease = StorageCoordinator::instance().acquireIo(
        storageOwner(), StorageAccess::read, 0);
    if (!lease) return;
    count = outgoingFile_.read(
        payload_, std::min<size_t>(kLinkMaxDataBytes,
                                   outgoingLength_ - outgoingRead_));
  }
  if (count == 0) {
    finishOutgoingFile(false);
    disconnect();
    return;
  }
  outgoingRead_ += count;
  const bool final = outgoingRead_ == outgoingLength_;
  if (!queueFrame(LinkFrameType::data, final ? 1 : 0,
                  outgoingRequestId_, payload_, count,
                  final ? TxCompletion::fileFinal :
                          TxCompletion::fileData)) {
    finishOutgoingFile(false);
    disconnect();
  }
}

void PokePodLinkService::finishOutgoingFile(bool success) {
  if (!outgoingCleanupPending_) {
    outgoingCleanupSuccess_ = success;
    if (!outgoingCleanup_.begin(outgoingFile_, nullptr, "",
                                outgoingStorageReservation_, storageOwner(),
                                StorageAccess::read)) return;
    outgoingCleanupPending_ = true;
  }
  cleanupOutgoingStorage();
}

bool PokePodLinkService::cleanupOutgoingStorage() {
  if (!outgoingCleanupPending_) return true;
  if (!outgoingCleanup_.poll()) return false;
  outgoingCleanupPending_ = false;
  finishOutgoingCleanup();
  return true;
}

void PokePodLinkService::finishOutgoingCleanup() {
  const bool success = outgoingCleanupSuccess_;
  const String resultTransaction = outgoingResultTransactionId_;
  outgoingPhase_ = OutgoingPhase::none;
  outgoingRequestId_ = 0;
  outgoingLength_ = 0;
  outgoingRead_ = 0;
  outgoingResultTransactionId_ = "";
  outgoingCleanupSuccess_ = false;
  if (success && !resultTransaction.isEmpty()) {
    maintenanceCompletion_.resultFetched(resultTransaction.c_str(), true);
  }
  if (!txStepper_.active() && pendingControlBytes_ == 0 &&
      releaseRequestLeaseWhenTxDrained_ && activeMaintenance_.isEmpty()) {
    releaseRequestLeaseNow();
  }
}

void PokePodLinkService::abortOutgoing() {
  txStepper_.cancel();
  txFrameBytes_ = 0;
  txCompletion_ = TxCompletion::none;
  pendingControlBytes_ = 0;
  pendingControlCompletion_ = TxCompletion::none;
  if (outgoingCleanupPending_) {
    // A socket cancellation before deferred handle cleanup completes is not a
    // confirmed result fetch, even if the final frame had left the TX buffer.
    outgoingCleanupSuccess_ = false;
    cleanupOutgoingStorage();
  } else if (outgoingPhase_ != OutgoingPhase::none || outgoingFile_) {
    finishOutgoingFile(false);
  } else {
    outgoingStorageReservation_.release();
    outgoingResultTransactionId_ = "";
  }
  releaseRequestLeaseWhenTxDrained_ = false;
}

void PokePodLinkService::onFrameSent(TxCompletion completion) {
  if (completion == TxCompletion::fileResponse) {
    if (outgoingLength_ == 0) finishOutgoingFile(true);
    else outgoingPhase_ = OutgoingPhase::data;
  } else if (completion == TxCompletion::fileFinal) {
    finishOutgoingFile(true);
  }
}

bool PokePodLinkService::drainLinkCapture() {
  if (captureRuntime_ == nullptr) return false;
  bool ok = true;
  AudioCaptureFrame frame;
  while (captureRuntime_->pop(frame)) {
    if (audio_ != nullptr) {
      audio_->observeCapturedMono(frame.samples,
                                  kAudioCaptureSamplesPerFrame);
    }
    if (recorder_ != nullptr && recorder_->recording() &&
        !recorder_->appendMono16(frame.samples,
                                 kAudioCaptureSamplesPerFrame, *log_)) {
      ok = false;
    }
  }
  return ok;
}

bool PokePodLinkService::stopLinkRecording(bool commit) {
  if (!linkOwnedRecording_) return false;
  const bool stopped = captureRuntime_ != nullptr &&
      captureRuntime_->stop(*log_);
  const bool drained = drainLinkCapture();
  const bool complete = commit && stopped && drained &&
      captureRuntime_ != nullptr && !captureRuntime_->incomplete();
  bool recorderFinished = false;
  if (recorder_ != nullptr && recorder_->recording()) {
    recorderFinished = complete
        ? recorder_->stop(*log_, RecorderStopReason::user)
        : recorder_->abortCapture(*log_);
  }
  if (captureRouter_ != nullptr) {
    captureRouter_->release(AudioCaptureOwner::localCapsule);
  }
  linkOwnedRecording_ = false;
  return complete && recorderFinished;
}

bool PokePodLinkService::transferPermitted() const {
  return linkTransferPermitted(transferGate_, millis());
}

uint32_t PokePodLinkService::storageIoTimeout() const {
  // A wireless service owns an external absolute deadline and must never wait
  // inside SD arbitration after its socket has been cancelled. USB has no
  // five-minute window and keeps the bounded compatibility wait.
  return transferGate_ == nullptr ? 1000U : 0U;
}

StorageOwner PokePodLinkService::storageOwner() const {
  if (commandStorageActive_) return StorageOwner::capsuleTransaction;
  return transport_ == LinkTransport::wifi ? StorageOwner::wifiLink
                                            : StorageOwner::usbLink;
}

void PokePodLinkService::rememberCompleted(uint32_t requestId) {
  if (requestId != 0) completed_.complete(requestId);
}

bool PokePodLinkService::ensureDirectoryTree(const String &path) {
  if (fs_ == nullptr || path.isEmpty() || path[0] != '/') return false;
  String current;
  for (size_t index = 1; index <= path.length(); ++index) {
    if (index == path.length() || path[index] == '/') {
      current = path.substring(0, index);
      if (!storageExists(current, StorageAccess::read) &&
          !storageMkdir(current)) return false;
    }
  }
  return true;
}

bool PokePodLinkService::storageExists(const String &path,
                                       StorageAccess access) const {
  if (fs_ == nullptr) return false;
  StorageIoLease lease = StorageCoordinator::instance().acquireIo(
      storageOwner(), access, 1000);
  return lease && fs_->exists(path);
}

bool PokePodLinkService::storageRename(const String &source,
                                       const String &target) {
  if (fs_ == nullptr) return false;
  StorageIoLease lease = StorageCoordinator::instance().acquireIo(
      storageOwner(), StorageAccess::mutation, storageIoTimeout());
  return lease && fs_->rename(source, target);
}

bool PokePodLinkService::storageRemove(const String &path) {
  if (fs_ == nullptr) return false;
  StorageIoLease lease = StorageCoordinator::instance().acquireIo(
      storageOwner(), StorageAccess::mutation, storageIoTimeout());
  return lease && (!fs_->exists(path) || fs_->remove(path));
}

bool PokePodLinkService::storageMkdir(const String &path) {
  if (fs_ == nullptr) return false;
  StorageIoLease lease = StorageCoordinator::instance().acquireIo(
      storageOwner(), StorageAccess::mutation, storageIoTimeout());
  return lease && (fs_->exists(path) || fs_->mkdir(path));
}

bool PokePodLinkService::storageRmdir(const String &path) {
  if (fs_ == nullptr) return false;
  StorageIoLease lease = StorageCoordinator::instance().acquireIo(
      storageOwner(), StorageAccess::mutation, storageIoTimeout());
  return lease && (!fs_->exists(path) || fs_->rmdir(path));
}

void PokePodLinkService::closeStorageFile(File &file,
                                          StorageAccess access) const {
  if (!file) return;
  StorageIoLease lease = StorageCoordinator::instance().acquireIo(
      storageOwner(), access, 0);
  if (lease) {
    file.close();
    return;
  }
  deferStorageFile(file, access);
}

bool PokePodLinkService::finishCopiedFiles(File &input, File &output) const {
  StorageIoLease lease = StorageCoordinator::instance().acquireIo(
      storageOwner(), StorageAccess::mutation, 0);
  if (lease) {
    bool ok = true;
    if (output) {
      output.flush();
      ok = output.getWriteError() == 0;
      output.close();
    }
    if (input) input.close();
    return ok;
  }
  deferStorageFile(output, StorageAccess::mutation, true);
  deferStorageFile(input, StorageAccess::read);
  return false;
}

void PokePodLinkService::deferStorageFile(File &file, StorageAccess access,
                                           bool flushBeforeClose) const {
  if (!file) return;
  deferredCommandFiles_.push_back(
      {file, storageOwner(), access, flushBeforeClose});
  // The queued copy owns the underlying handle. Clearing this reference does
  // not close it; the last reference is released under a later physical lease.
  file = File();
}

bool PokePodLinkService::stepDeferredFileCleanup() {
  if (deferredCommandFiles_.empty()) return true;
  DeferredCommandFile &pending = deferredCommandFiles_.front();
  StorageIoLease lease = StorageCoordinator::instance().acquireIo(
      pending.owner, pending.access, 0);
  if (!lease) return false;
  if (pending.file) {
    if (pending.flushBeforeClose) {
      pending.file.flush();
      if (pending.file.getWriteError() != 0) {
        deferredCommandFileFailed_ = true;
      }
    }
    pending.file.close();
  }
  deferredCommandFiles_.erase(deferredCommandFiles_.begin());
  return deferredCommandFiles_.empty();
}

bool PokePodLinkService::writeTextAtomic(const String &path,
                                         const String &text) {
  return transaction_.writeTextAtomic(
      path, text, storageOwner(), "link-metadata");
}

bool PokePodLinkService::validFontFile(const String &path) const {
  StorageIoLease lease = StorageCoordinator::instance().acquireIo(
      storageOwner(), StorageAccess::read, storageIoTimeout());
  if (!lease) return false;
  File file = fs_->open(path, FILE_READ);
  uint8_t header[kFontHeaderBytes];
  if (!file || file.isDirectory() ||
      file.read(header, sizeof(header)) != sizeof(header)) {
    if (file) file.close();
    return false;
  }
  const uint32_t count = linkGet32(header + 12);
  const uint32_t entrySize = linkGet32(header + 16);
  const bool ok = memcmp(header, "PKF2", 4) == 0 &&
      validFontLayout(linkGet16(header + 4), linkGet16(header + 6),
                      header[8], count, entrySize, file.size());
  file.close();
  return ok;
}

String PokePodLinkService::readText(const String &path, size_t limit) const {
  if (fs_ == nullptr) return String();
  StorageIoLease lease = StorageCoordinator::instance().acquireIo(
      storageOwner(), StorageAccess::read, storageIoTimeout());
  if (!lease) return String();
  File file = fs_->open(path, FILE_READ);
  if (!file || file.isDirectory() || file.size() > limit) {
    if (file) file.close();
    return String();
  }
  String value;
  if (!value.reserve(file.size() + 1)) {
    file.close();
    return String();
  }
  while (file.available()) value += static_cast<char>(file.read());
  file.close();
  return value;
}

bool PokePodLinkService::collectFiles(const String &directory,
                                      const String &relative, uint8_t depth,
                                      std::vector<String> &files) const {
  if (!transferPermitted() || depth > 5 || files.size() >= 2048) return false;
  File root;
  {
    StorageIoLease lease = StorageCoordinator::instance().acquireIo(
        storageOwner(), StorageAccess::read, storageIoTimeout());
    if (!lease) return false;
    root = fs_->open(directory);
    if (!root || !root.isDirectory()) {
      if (root) root.close();
      return false;
    }
  }
  while (transferPermitted()) {
    String full;
    bool isDirectory = false;
    bool hasEntry = false;
    {
      StorageIoLease lease = StorageCoordinator::instance().acquireIo(
          storageOwner(), StorageAccess::read, storageIoTimeout());
      if (!lease) {
        closeStorageFile(root, StorageAccess::read);
        return false;
      }
      File entry = root.openNextFile();
      hasEntry = static_cast<bool>(entry);
      if (hasEntry) {
        full = entry.name();
        isDirectory = entry.isDirectory();
        entry.close();
      }
    }
    if (!hasEntry) break;
    if (!transferPermitted()) {
      closeStorageFile(root, StorageAccess::read);
      return false;
    }
    const int slash = full.lastIndexOf('/');
    const String name = slash >= 0 ? full.substring(slash + 1) : full;
    const String childRelative = relative.isEmpty() ? name : relative + "/" + name;
    if (isDirectory) {
      if (!hiddenReadDenied(childRelative)) {
        if (!collectFiles(directory + "/" + name, childRelative,
                          depth + 1, files)) {
          closeStorageFile(root, StorageAccess::read);
          return false;
        }
      }
    } else if (!hiddenReadDenied(childRelative)) {
      files.push_back(childRelative);
    }
  }
  closeStorageFile(root, StorageAccess::read);
  return transferPermitted();
}

String PokePodLinkService::deviceId() const {
  char value[24];
  formatPokePodDeviceId(ESP.getEfuseMac(), value);
  return String(value);
}

bool PokePodLinkService::acquireRequestLease(uint32_t requestId) {
  if (requestLeaseHeld_) return true;
  if (coordinator_ == nullptr || transport_ == LinkTransport::none) {
    requestLeaseHeld_ = true;
    return true;
  }
  if (!coordinator_->acquire(transport_)) {
    sendBusy(requestId, 250);
    return false;
  }
  requestLeaseHeld_ = true;
  return true;
}

void PokePodLinkService::releaseRequestLease() {
  if (!requestLeaseHeld_) return;
  if (txStepper_.active() || pendingControlBytes_ != 0 ||
      outgoingPhase_ != OutgoingPhase::none) {
    releaseRequestLeaseWhenTxDrained_ = true;
    return;
  }
  releaseRequestLeaseNow();
}

void PokePodLinkService::releaseRequestLeaseNow() {
  if (!requestLeaseHeld_) {
    releaseRequestLeaseWhenTxDrained_ = false;
    return;
  }
  if (coordinator_ != nullptr && transport_ != LinkTransport::none) {
    coordinator_->release(transport_);
  }
  requestLeaseHeld_ = false;
  releaseRequestLeaseWhenTxDrained_ = false;
}

bool PokePodLinkService::foregroundBusy() const {
  if (recorder_ == nullptr || tencent_ == nullptr || bleVoice_ == nullptr ||
      captureRouter_ == nullptr) return true;
  if (audio_ != nullptr && audio_->playing()) return true;
  return linkForegroundBusy(recorder_->recording(), tencent_->working(),
                            bleVoice_->streaming(),
                            captureRouter_->available());
}

}  // namespace pokepod
