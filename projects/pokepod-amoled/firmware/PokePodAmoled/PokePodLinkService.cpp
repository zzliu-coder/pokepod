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
constexpr size_t kMaxCommandJsonBytes = 64U * 1024U;
constexpr size_t kCommandReadBytesPerPoll = 4096U;
constexpr size_t kReadPageFiles = 12;
constexpr size_t kLinkTxFrameBytes = kLinkHeaderBytes + kLinkMaxDataBytes;
constexpr size_t kLinkPendingControlBytes =
    kLinkHeaderBytes + kLinkMaxControlBytes;
constexpr size_t kLinkWriteSliceBytes = 512;
constexpr const char *kLinkCommandUploadPart =
    "/PokeCapsule/.system/commands/incoming/upload.part";
constexpr const char *kLinkStagedUploadPart =
    "/PokeCapsule/.staging/link-upload.part";
constexpr const char *kLinkFontUploadPart =
    "/PokeCapsule/.system/fonts/cjk20.a4.part";
constexpr uint8_t kBatchPlanNoOp = 1U << 0;
constexpr uint8_t kBatchPlanCapsuleBackup = 1U << 1;
constexpr uint8_t kBatchPlanTrashBackup = 1U << 2;
constexpr uint8_t kBatchPlanProcessingBackup = 1U << 3;

bool batchOperation(const char *operation) {
  return capsuleBatchOperation(operation);
}

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

void appendJsonKey(String &json, const char *key) {
  if (!json.isEmpty()) json += ',';
  json += '"';
  json += key;
  json += "\":";
}

void appendJsonBool(String &json, const char *key, bool value) {
  appendJsonKey(json, key);
  json += value ? "true" : "false";
}

void appendJsonNumber(String &json, const char *key, int64_t value) {
  appendJsonKey(json, key);
  char encoded[24];
  snprintf(encoded, sizeof(encoded), "%lld",
           static_cast<long long>(value));
  json += encoded;
}

void appendJsonString(String &json, const char *key, const String &value) {
  appendJsonKey(json, key);
  json += '"';
  json += jsonEscaped(value);
  json += '"';
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
  quiesceRequested_ = false;
  if (!transaction_.begin(fs, log)) return false;
  if (!transactionRunner_.begin(fs, log)) return false;
  if (!batchJournalStore_.begin(fs)) return false;
  startupPartCleanupIndex_ = 0;
  startupPartCleanupFailures_ = 0;
  startupPartCleanupPending_ = false;
  startupReady_ = false;
  startupRecoveryFailed_ = false;
  startupBatchCandidateInvalid_ = false;
  mutationRecoveryBlocked_ = false;
  startupPurgePending_ = true;
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
      !ensureDirectoryTree(String(kCapsuleSystem) + "/commands/incoming") ||
      !ensureDirectoryTree(CapsuleBatchJournalStore::kDirectory)) {
    return false;
  }
  // Recover durable transactions before sweeping the three fixed Link upload
  // parts. A cut after journal publication but before publishPrepared must let
  // the durable journal choose old/new state before the borrowed part is
  // removed. No transport frames are accepted until both phases finish.
  if (!transactionRunner_.startRecovery(StorageOwner::capsuleTransaction)) {
    return false;
  }
  transactionPurpose_ = TransactionPurpose::startupRecovery;
  return true;
}

void PokePodLinkService::disconnect() {
  if (batchExecutor_.active()) abandonBatchCommand();
  manifestResponseRequestId_ = 0;
  manifestResponseJson_ = "";
  manifestFailureRequestId_ = 0;
  manifestFailureMessage_ = "";
  if (linkRecordingStop_.active()) {
    linkRecordingStop_.suppressResponseAndAbort();
  } else if (linkOwnedRecording_) {
    (void)requestLinkRecordingStop(0, false, false);
  }
  if (linkOwnedRecording_) transactionGate_.cancel();
  if (transactionPurpose_ == TransactionPurpose::incoming ||
      transactionPurpose_ == TransactionPurpose::commandText) {
    if (transactionRunner_.active()) transactionGate_.cancel();
    transactionRespond_ = false;
    commandTextRespond_ = false;
  } else if (transactionPurpose_ == TransactionPurpose::commandTextResult ||
             transactionPurpose_ ==
                 TransactionPurpose::commandFailureResult) {
    commandTextRespond_ = false;
  }
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
  commandLoadRespond_ = false;
  resetFrame();
  releaseRequestLeaseNow();
}

void PokePodLinkService::requestQuiesce() {
  if (quiesceRequested_) return;
  quiesceRequested_ = true;
  disconnect();
}

bool PokePodLinkService::quiesced() const {
  return quiesceRequested_ && !sessionActive_ &&
      commandLoadState_ == CommandLoadState::none &&
      incomingKind_ == IncomingKind::none && !incomingCleanupPending_ &&
      outgoingPhase_ == OutgoingPhase::none && !outgoingCleanupPending_ &&
      !manifestStepper_.active() && !manifestCleanupPending_ &&
      !transactionRunner_.active() &&
      transactionPurpose_ == TransactionPurpose::none &&
      !batchExecutor_.active() && batchPending_ == BatchPending::none &&
      deferredCommandFiles_.empty() && deferredTreeCleanupStack_.empty() &&
      !commandCleanupPending_ && !commandStorageActive_ &&
      !linkOwnedRecording_ && !linkRecordingStop_.active() &&
      !requestLeaseHeld_;
}

void PokePodLinkService::pollDeferredCleanup() {
  advanceCommandLoad();
  advanceBatchCommand();
  advanceTransactionRunner();
  advanceBatchStartupRecovery();
  advanceStartupPartCleanup();
  cleanupPurgeStaging();
  advanceLinkRecordingStart();
  advanceLinkRecordingStop();
  if (incomingCleanupPending_) cleanupIncomingStorage();
  if (outgoingCleanupPending_) cleanupOutgoingStorage();
  if (manifestCleanupPending_) cleanupManifestStorage();
  stepDeferredFileCleanup();
  if (!startupPurgePending_ && deferredCommandFiles_.empty()) {
    stepDeferredTreeCleanup();
  }
  finishCommandStorageCleanup();
  if (!startupReady_ && !startupRecoveryFailed_ &&
      !startupBatchRecoveryPending_ && !startupPartCleanupPending_ &&
      !startupPurgePending_ && deferredTreeCleanupStack_.empty()) {
    startupReady_ = true;
  }
}

void PokePodLinkService::poll(uint32_t nowMs) {
  pollDeferredCleanup();
  if (quiesceRequested_) return;
  if (!startupReady_) return;
  if (commandLoadState_ != CommandLoadState::none ||
      batchExecutor_.active() || transactionRunner_.active() ||
      transactionPurpose_ != TransactionPurpose::none) return;
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
  if (linkOwnedRecording_ && !linkRecordingStop_.active()) {
    const bool captureOk = drainLinkCapture();
    if (!captureOk) {
      (void)requestLinkRecordingStop(0, false, false);
    } else if (recorder_ != nullptr && recorder_->stopRequested()) {
      (void)requestLinkRecordingStop(0, true, false);
    }
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
  // A retransmission of an in-flight recording request shares the original
  // terminal response. Sending a second response here would race the storage
  // ACK and could let the client release the session before admission ended.
  if (requestId != 0 &&
      (linkRecordingStart_.ownsRequest(requestId) ||
       linkRecordingStop_.ownsRequest(requestId))) return;
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
    const String temporaryPath = kLinkFontUploadPart;
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
      const String temporaryPath = kLinkStagedUploadPart;
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
    if (binaryLength > static_cast<int64_t>(kMaxCommandJsonBytes)) {
      cJSON_Delete(root);
      sendError(requestId, "command is too large");
      rememberCompleted(requestId);
      releaseRequestLease();
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
      rememberCompleted(requestId);
      releaseRequestLease();
    }
    return;
  }

  if (acquireRequestLease(requestId)) handleImmediate(requestId, root);
  cJSON_Delete(root);
  if (manifestRequestId_ == requestId && manifestStepper_.active()) return;
  if (linkRecordingStart_.ownsRequest(requestId)) return;
  if (linkRecordingStop_.ownsRequest(requestId)) return;
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
  if (fs_->exists(temporaryPath) && !fs_->remove(temporaryPath)) {
    incomingStorageReservation_.release();
    return false;
  }
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
  const char *key = transaction.isEmpty() ? finalPath.c_str() :
                                            transaction.c_str();
  if (!valid || !transactionRunner_.startPreparedFile(
                    key, temporary, finalPath, storageOwner())) {
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
  transactionGate_.beginOperation(transferGate_);
  transactionPurpose_ = TransactionPurpose::incoming;
  transactionIncomingKind_ = kind;
  transactionRequestId_ = requestId;
  transactionPreparedPath_ = temporary;
  transactionFinalPath_ = finalPath;
  transactionId_ = transaction;
  transactionRespond_ = sessionActive_ && transferPermitted();
}

void PokePodLinkService::advanceTransactionRunner() {
  if (!transactionRunner_.active() ||
      transactionPurpose_ == TransactionPurpose::none) return;
  CapsuleTransactionGate *gate =
      (transactionPurpose_ == TransactionPurpose::incoming ||
       transactionPurpose_ == TransactionPurpose::commandText)
          ? &transactionGate_ : nullptr;
  const CapsuleTransactionPollResult result = transactionRunner_.poll(
      millis(), gate);
  if (result == CapsuleTransactionPollResult::progress ||
      result == CapsuleTransactionPollResult::wouldBlock) return;
  if (transactionPurpose_ == TransactionPurpose::startupRecovery) {
    finishStartupRecovery(result ==
                          CapsuleTransactionPollResult::recovered);
  } else if (transactionPurpose_ == TransactionPurpose::incoming) {
    finishIncomingTransaction(result ==
                              CapsuleTransactionPollResult::committed);
  } else if (transactionPurpose_ == TransactionPurpose::commandText) {
    finishTextCommand(result == CapsuleTransactionPollResult::committed);
  } else if (transactionPurpose_ == TransactionPurpose::commandTextResult) {
    finishTextResult(result == CapsuleTransactionPollResult::committed);
  } else if (transactionPurpose_ ==
             TransactionPurpose::commandFailureResult) {
    finishCommandFailureResult(
        result == CapsuleTransactionPollResult::committed);
  }
}

void PokePodLinkService::finishStartupRecovery(bool recovered) {
  if (!recovered) {
    startupRecoveryFailed_ = true;
    if (log_ != nullptr) {
      log_->println("{\"event\":\"link_startup_recovery_failed\"}");
    }
    return;
  }
  transactionPurpose_ = TransactionPurpose::none;
  startupBatchRecoveryPending_ = true;
}

bool PokePodLinkService::startBatchStartupRecovery(
    const String &transactionId) {
  constexpr StorageOwner owner = StorageOwner::capsuleTransaction;
  commandStorageReservation_ = StorageCoordinator::instance().reserve(
      owner, StorageAccess::mutation, 0);
  if (!commandStorageReservation_) return false;
  StoredCapsuleBatchState state;
  const CapsuleBatchJournalStore::LoadResult loaded =
      batchJournalStore_.loadStatus(transactionId, owner, state);
  startupBatchCandidateInvalid_ =
      loaded == CapsuleBatchJournalStore::LoadResult::invalid;
  if (loaded != CapsuleBatchJournalStore::LoadResult::loaded) {
    commandStorageReservation_.release();
    return false;
  }
  prepareCapsuleBatchRecovery(state);
  state.flags &= ~capsuleBatchResponseAllowed;
  if (state.phase == CapsuleBatchPhase::preflight) {
    state.flags &= ~capsuleBatchSuccess;
    state.cursor = state.applied = 0;
    state.phase = CapsuleBatchPhase::result;
  } else if (state.phase == CapsuleBatchPhase::apply) {
    state.flags &= ~capsuleBatchSuccess;
    state.cursor = state.applied;
    state.phase = state.cursor == 0 &&
        (state.flags & capsuleBatchFinalizeApplied) == 0
        ? CapsuleBatchPhase::result : CapsuleBatchPhase::rollback;
  }
  sealCapsuleBatchState(state);
  if (!batchJournalStore_.checkpoint(state, owner)) {
    commandStorageReservation_.release();
    return false;
  }

  LinkCommandExecutor::Phase phase = LinkCommandExecutor::Phase::cleanup;
  if (state.phase == CapsuleBatchPhase::rollback) {
    phase = LinkCommandExecutor::Phase::rollback;
  } else if (state.phase == CapsuleBatchPhase::result) {
    phase = LinkCommandExecutor::Phase::result;
  }
  const bool folderFinalize =
      strcmp(state.operation, "deleteFolderToInbox") == 0;
  if (!batchExecutor_.resume(
          state.total, phase, state.cursor, state.applied,
          (state.flags & capsuleBatchSuccess) != 0,
          (state.flags & capsuleBatchRollbackFailed) != 0,
          folderFinalize,
          (state.flags & capsuleBatchFinalizeApplied) != 0)) {
    commandStorageReservation_.release();
    return false;
  }
  commandStorageActive_ = true;
  batchJournalState_ = state;
  batchTransactionId_ = transactionId;
  batchCommandPath_ = String(kCapsuleSystem) + "/commands/incoming/" +
      transactionId + ".json";
  batchRequestId_ = 0;
  batchJsonRoot_ = nullptr;
  batchMessage_ = (state.flags & capsuleBatchSuccess) != 0
      ? "committed" : "interrupted batch rolled back";
  batchPending_ = BatchPending::none;
  transactionGate_.reset();
  return true;
}

void PokePodLinkService::quarantineBatchJournal(
    const String &transactionId) {
  char suffix[24];
  snprintf(suffix, sizeof(suffix), ".blocked-%08lx",
           static_cast<unsigned long>(esp_random()));
  (void)batchJournalStore_.quarantine(
      transactionId, suffix, storageOwner());
  if (log_ != nullptr) {
    log_->println("{\"event\":\"link_batch_journal_blocked\"}");
  }
}

void PokePodLinkService::advanceBatchStartupRecovery() {
  if (!startupBatchRecoveryPending_ || startupRecoveryFailed_ ||
      batchExecutor_.active()) return;
  if (!startupBatchCandidate_.isEmpty()) {
    if (startBatchStartupRecovery(startupBatchCandidate_)) {
      startupBatchCandidate_ = "";
      startupBatchCandidateFailures_ = 0;
    } else if (startupBatchCandidateInvalid_) {
      quarantineBatchJournal(startupBatchCandidate_);
      startupBatchCandidate_ = "";
      startupBatchCandidateFailures_ = 0;
      startupBatchCandidateInvalid_ = false;
    } else if (++startupBatchCandidateFailures_ >= 3) {
      // An IO/checkpoint failure is still authoritative.  Keep the original
      // journal and fail closed; quarantining it would permit later mutation
      // to be overwritten by a delayed rollback on the next boot.
      startupRecoveryFailed_ = true;
    }
    return;
  }
  if (!startupBatchReservation_) {
    startupBatchReservation_ = StorageCoordinator::instance().reserve(
        storageOwner(), StorageAccess::mutation, 0);
    if (!startupBatchReservation_) return;
  }
  StorageIoLease lease = StorageCoordinator::instance().acquireIo(
      storageOwner(), StorageAccess::mutation, 0);
  if (!lease) return;
  if (!startupBatchDirectory_) {
    startupBatchDirectory_ = fs_->open(CapsuleBatchJournalStore::kDirectory);
    if (!startupBatchDirectory_ || !startupBatchDirectory_.isDirectory()) {
      if (startupBatchDirectory_) startupBatchDirectory_.close();
      startupBatchReservation_.release();
      startupBatchRecoveryPending_ = false;
      startupPartCleanupPending_ = true;
      return;
    }
  }
  File entry = startupBatchDirectory_.openNextFile();
  if (!entry) {
    startupBatchDirectory_.close();
    startupBatchReservation_.release();
    startupBatchRecoveryPending_ = false;
    startupPartCleanupPending_ = true;
    return;
  }
  const String full = entry.name();
  entry.close();
  const int slash = full.lastIndexOf('/');
  const String name = slash < 0 ? full : full.substring(slash + 1);
  if (!name.endsWith(".cbj")) return;
  const String transactionId = name.substring(0, name.length() - 4);
  startupBatchDirectory_.close();
  startupBatchReservation_.release();
  lease.release();
  if (capsuleBatchUuid(transactionId.c_str())) {
    startupBatchCandidate_ = transactionId;
    startupBatchCandidateFailures_ = 0;
  } else {
    const String source = String(CapsuleBatchJournalStore::kDirectory) +
        "/" + name;
    char suffix[24];
    snprintf(suffix, sizeof(suffix), ".blocked-%08lx",
             static_cast<unsigned long>(esp_random()));
    (void)storageRename(source, source + suffix);
  }
}

void PokePodLinkService::advanceStartupPartCleanup() {
  if (!startupPartCleanupPending_ || startupReady_ ||
      startupRecoveryFailed_) return;
  if (!startupPartCleanupReservation_) {
    startupPartCleanupReservation_ = StorageCoordinator::instance().reserve(
        storageOwner(), StorageAccess::mutation, 0);
    if (!startupPartCleanupReservation_) return;
  }
  static constexpr const char *parts[] = {
      kLinkCommandUploadPart, kLinkStagedUploadPart, kLinkFontUploadPart};
  if (startupPartCleanupIndex_ >= sizeof(parts) / sizeof(parts[0])) {
    startupPartCleanupReservation_.release();
    startupPartCleanupPending_ = false;
    return;
  }
  StorageIoLease lease = StorageCoordinator::instance().acquireIo(
      storageOwner(), StorageAccess::mutation, 0);
  if (!lease) return;
  const char *path = parts[startupPartCleanupIndex_];
  if (fs_->exists(path) && !fs_->remove(path)) {
    if (++startupPartCleanupFailures_ >= 3) {
      startupRecoveryFailed_ = true;
      startupPartCleanupPending_ = false;
      startupPartCleanupReservation_.release();
      if (log_ != nullptr) {
        log_->println("{\"event\":\"link_startup_part_cleanup_failed\"}");
      }
    }
    return;
  }
  startupPartCleanupFailures_ = 0;
  ++startupPartCleanupIndex_;
}

void PokePodLinkService::finishIncomingTransaction(bool committed) {
  const IncomingKind kind = transactionIncomingKind_;
  const uint32_t requestId = transactionRequestId_;
  const String preparedPath = transactionPreparedPath_;
  const String finalPath = transactionFinalPath_;
  const String transaction = transactionId_;
  const bool respond = transactionRespond_ && sessionActive_ &&
      transferPermitted();
  transactionPurpose_ = TransactionPurpose::none;
  transactionIncomingKind_ = IncomingKind::none;
  transactionRequestId_ = 0;
  transactionPreparedPath_ = "";
  transactionFinalPath_ = "";
  transactionId_ = "";
  transactionRespond_ = false;
  transactionGate_.reset();
  if (!committed) {
    incomingCleanupRespond_ = respond;
    incomingCleanupRequestId_ = requestId;
    incomingCleanupMessage_ = "atomic file commit failed";
    incomingCleanupPending_ = incomingCleanup_.begin(
        incomingFile_, fs_, preparedPath, incomingStorageReservation_,
        storageOwner(), StorageAccess::mutation);
    if (incomingCleanupPending_) cleanupIncomingStorage();
    return;
  }
  incomingStorageReservation_.release();
  if (kind == IncomingKind::command) {
    handleCommandFile(requestId, finalPath, transaction);
    if (commandCleanupPending_) return;
  } else if (respond && kind == IncomingKind::systemFont) {
    sendOk(requestId, "\"installed\":true,\"rebootRequired\":true");
  } else if (respond) {
    sendOk(requestId);
  }
  if (respond) {
    rememberCompleted(requestId);
    if (activeMaintenance_.isEmpty()) releaseRequestLease();
  }
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
    String extra;
    extra.reserve(3072);
    appendJsonBool(extra, "recording", recorder_->recording());
    appendJsonBool(extra, "transcribing", tencent_->working());
    appendJsonNumber(extra, "batteryPercent", status.batteryPercent);
    appendJsonBool(extra, "charging", status.charging);
    appendJsonString(extra, "wifi", wifi_->phaseName());
    appendJsonNumber(extra, "wifiDisconnectReason",
                     wifi_->lastDisconnectReason());
    appendJsonString(extra, "wifiDisconnectKind",
                     wifiFailureKey(wifi_->lastDisconnectReason()));
    appendJsonBool(extra, "wifiRadioOn", wifi_->radioOn());
    appendJsonBool(extra, "wifiPowerSave", wifi_->powerSaveEnabled());
    appendJsonNumber(extra, "wifiPowerSaveError", wifi_->powerSaveError());
    appendJsonNumber(extra, "pendingCapsules", library_->pendingCount());
    appendJsonNumber(extra, "asr_hash_ms", tencent_->lastHashElapsedMs());
    appendJsonNumber(extra, "asr_connect_ms",
                     tencent_->lastConnectElapsedMs());
    appendJsonNumber(extra, "asr_upload_ms",
                     tencent_->lastUploadElapsedMs());
    appendJsonNumber(extra, "asr_total_ms", tencent_->lastTotalElapsedMs());
    appendJsonString(extra, "asr_last_code", tencent_->lastCode());
    appendJsonNumber(extra, "asr_tls_error", tencent_->lastNetworkError());
    appendJsonString(extra, "asr_tls_detail",
                     tencent_->lastNetworkErrorDetail());
    appendJsonNumber(extra, "asr_heap_free_before_tls",
                     tencent_->lastInternalHeapFreeBeforeTls());
    appendJsonNumber(extra, "asr_heap_largest_before_tls",
                     tencent_->lastInternalHeapLargestBeforeTls());
    appendJsonNumber(extra, "asr_psram_free_before_tls",
                     tencent_->lastPsramFreeBeforeTls());
    appendJsonBool(extra, "sdReady", status.sdCard);
    if (capabilities_ != nullptr) {
      appendJsonNumber(extra, "capabilityObservedMask",
                       capabilities_->observedMask());
      appendJsonNumber(extra, "capabilityReadyMask",
                       capabilities_->readyMask());
      appendJsonBool(extra, "capsuleLibraryReady",
                     capabilities_->ready(DeviceCapability::capsuleLibrary));
      appendJsonBool(extra, "recorderReady",
                     capabilities_->ready(DeviceCapability::recording));
      appendJsonBool(extra, "asrWorkerReady",
                     capabilities_->ready(DeviceCapability::transcription));
    }
    appendJsonString(extra, "variant", variantName(status.variant));
    appendJsonBool(extra, "ioExpander", status.ioExpander);
    appendJsonBool(extra, "display", status.display);
    appendJsonBool(extra, "touch", status.touch);
    appendJsonBool(extra, "rtc", status.rtc);
    appendJsonBool(extra, "imu", status.imu);
    appendJsonBool(extra, "pmu", status.pmu);
    appendJsonBool(extra, "vbusPresent", status.vbusPresent);
    appendJsonBool(extra, "screenOn", status.screenOn);
    appendJsonBool(extra, "audio", audio_ != nullptr && audio_->ready());
    appendJsonBool(extra, "usb", usb_->ready());
    appendJsonBool(extra, "host_connected", usb_->hostConnected());
    appendJsonBool(extra, "bleVoiceConnected",
                   bleVoice_ != nullptr && bleVoice_->connected());
    appendJsonBool(extra, "bleVoiceReady",
                   bleVoice_ != nullptr && bleVoice_->appReady());
    appendJsonNumber(extra, "bleVoiceMtu",
                     bleVoice_ == nullptr ? 0 : bleVoice_->mtu());
    const BleVoiceQualitySnapshot bleQuality = bleVoice_ == nullptr
        ? BleVoiceQualitySnapshot() : bleVoice_->quality();
    appendJsonNumber(extra, "bleVoiceNotifyAttempts",
                     bleQuality.notifyAttempts);
    appendJsonNumber(extra, "bleVoiceNotifyAccepted",
                     bleQuality.notifyAccepted);
    appendJsonNumber(extra, "bleVoiceNotifyFailures",
                     bleQuality.notifyFailures);
    appendJsonNumber(extra, "bleVoiceQueueOverflows",
                     bleQuality.queueOverflows);
    appendJsonNumber(extra, "bleVoiceSessionFailures",
                     bleQuality.sessionFailures);
    appendJsonNumber(extra, "bleVoiceReadyTimeouts", bleQuality.readyTimeouts);
    appendJsonNumber(extra, "bleVoiceStopAckTimeouts",
                     bleQuality.stopAckTimeouts);
    appendJsonNumber(extra, "bleVoiceStreamTimeouts",
                     bleQuality.streamTimeouts);
    appendJsonNumber(extra, "bleVoiceLastErrorCode",
                     bleQuality.lastErrorCode);
    appendJsonNumber(extra, "audio_read_bytes", audio_->bytesRead());
    appendJsonNumber(extra, "audio_read_failures", audio_->readFailures());
    appendJsonNumber(extra, "audio_peak", audio_->peakSample());
    appendJsonString(extra, "playback_last_error", audio_->lastPlaybackError());
    appendJsonNumber(extra, "playback_start_failures",
                     audio_->playbackStartFailures());
    appendJsonNumber(extra, "playback_heap_largest_before_start",
                     audio_->playbackHeapLargestBeforeStart());
    appendJsonNumber(extra, "playback_file_reads",
                     audio_->playbackFileReadCount());
    appendJsonNumber(extra, "playback_pump_count",
                     audio_->playbackPumpCount());
    appendJsonNumber(extra, "playback_max_file_read_us",
                     audio_->playbackMaxFileReadUs());
    const AudioFrontEndMetrics &frontEnd = recorder_->audioMetrics();
    appendJsonString(extra, "audio_frontend_channel",
                     audioInputChannelName(frontEnd.selectedChannel));
    appendJsonNumber(extra, "audio_frontend_left_peak", frontEnd.leftPeak);
    appendJsonNumber(extra, "audio_frontend_right_peak", frontEnd.rightPeak);
    appendJsonNumber(extra, "audio_frontend_output_peak", frontEnd.outputPeak);
    appendJsonNumber(extra, "audio_frontend_noise_floor",
                     frontEnd.estimatedNoiseFloor);
    appendJsonNumber(extra, "audio_frontend_suppressed_samples",
                     frontEnd.suppressedSamples);
    appendJsonNumber(extra, "audio_frontend_limited_samples",
                     frontEnd.limitedSamples);
    appendJsonNumber(extra, "audio_frontend_max_gain_q12",
                     frontEnd.maximumGainQ12);
    appendJsonBool(extra, "tencentConfigured", config_->hasTencent());
    appendJsonNumber(extra, "wifiNetworkCount", config_->wifiNetworks().size());
    appendJsonNumber(extra, "ui_full_redraws", dashboard_->fullRedrawCount());
    appendJsonNumber(extra, "ui_body_redraws", dashboard_->bodyRedrawCount());
    appendJsonNumber(extra, "ui_partial_redraws",
                     dashboard_->partialRedrawCount());
    appendJsonNumber(extra, "ui_scroll_frame_last_us",
                     dashboard_->scrollFrameLastUs());
    appendJsonNumber(extra, "ui_scroll_frame_max_us",
                     dashboard_->scrollFrameMaxUs());
    appendJsonNumber(extra, "ui_scroll_compose_max_us",
                     dashboard_->scrollComposeMaxUs());
    appendJsonNumber(extra, "ui_scroll_transfer_max_us",
                     dashboard_->scrollTransferMaxUs());
    appendJsonNumber(extra, "ui_scroll_frames_over_budget",
                     dashboard_->scrollFramesOverBudget());
    appendJsonNumber(extra, "capsule_full_scans", library_->fullScanCount());
    appendJsonNumber(extra, "capsule_incremental_refreshes",
                     library_->incrementalRefreshCount());
    appendJsonNumber(extra, "capsule_refresh_fallbacks",
                     library_->refreshFallbackCount());
    appendJsonNumber(extra, "capsule_last_scan_us", library_->lastScanUs());
    appendJsonNumber(extra, "capsule_max_scan_us", library_->maxScanUs());
    appendJsonBool(extra, "ui_frame_buffer", dashboard_->frameBufferReady());
    appendJsonBool(extra, "ui_animation_buffer",
                   dashboard_->animationBufferReady());
    const RuntimePowerSnapshot &power = power_->snapshot();
    appendJsonString(extra, "powerMode", powerModeName(power.mode));
    appendJsonNumber(extra, "cpuMhz", power.cpuMhz);
    appendJsonNumber(extra, "powerTransitions", power.transitions);
    appendJsonNumber(extra, "lightSleepAttempts", power.lightSleepAttempts);
    appendJsonNumber(extra, "lightSleepCount", power.lightSleepCount);
    appendJsonNumber(extra, "lightSleepFailures", power.lightSleepFailures);
    appendJsonNumber(extra, "lightSleepMs", power.lightSleepUs / 1000ULL);
    appendJsonNumber(extra, "deepSleepWakeCount", power.deepSleepWakeCount);
    appendJsonNumber(extra, "deepSleepArmAttempts", power.deepSleepArmAttempts);
    appendJsonNumber(extra, "deepSleepArmFailures", power.deepSleepArmFailures);
    appendJsonBool(extra, "wokeFromDeepSleep", power.wokeFromDeepSleep);
    appendJsonBool(extra, "deepSleepTouchWakeArmed",
                   power.deepSleepTouchWakeArmed);
    appendJsonNumber(extra, "lastWakeCause", power.lastWakeCause);
    appendJsonNumber(extra, "wakeCauses", power.wakeCauses);
    const PowerDiagnosticsSnapshot &powerDiagnostic =
        powerDiagnostics_->snapshot();
    appendJsonNumber(extra, "powerActiveFacts", powerDiagnostic.activeFacts);
    appendJsonNumber(extra, "powerLightBlockers", powerDiagnostic.lightBlockers);
    appendJsonNumber(extra, "powerDeepBlockers", powerDiagnostic.deepBlockers);
    appendJsonNumber(extra, "powerCurrentBlockers",
                     powerDiagnostic.currentBlockers);
    appendJsonNumber(extra, "powerIdleMs", powerDiagnostic.idleMs);
    appendJsonNumber(extra, "powerDiagnosticCount", powerDiagnostics_->count());
    appendJsonNumber(extra, "powerDiagnosticPersistFailures",
                     powerDiagnostic.persistFailures);
    appendJsonNumber(extra, "powerAutomaticScreenWakes",
                     powerDiagnostic.automaticScreenWakes);
    appendJsonNumber(extra, "resetReason", esp_reset_reason());
    appendJsonNumber(extra, "internalHeapFree",
                     heap_caps_get_free_size(MALLOC_CAP_INTERNAL |
                                             MALLOC_CAP_8BIT));
    appendJsonNumber(extra, "internalHeapLargest",
                     heap_caps_get_largest_free_block(
                         MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    appendJsonNumber(extra, "psramFree", ESP.getFreePsram());
    appendJsonBool(extra, "automaticPmSupported", power.automaticPmSupported);
    appendJsonBool(extra, "bleModemSleepSupported",
                   power.bleModemSleepSupported);
    appendJsonNumber(extra, "provisioningDiagnosticCount",
                     provisioningDiagnostics_->count());
    appendJsonString(extra, "provisioningStartupPhase",
                     provisioningCoordinator_ == nullptr
                         ? "unavailable" : provisioningCoordinator_->phaseName());
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
      const RecorderOperationOwner recorderOwner =
          transport_ == LinkTransport::wifi
              ? RecorderOperationOwner::linkWifi
              : RecorderOperationOwner::linkUsb;
      if (acquired) transactionGate_.beginOperation(transferGate_);
      const bool recorderStartAccepted = acquired &&
          recorder_->requestStart(*log_, id, board_->utcNow(), space,
                                  recorderOwner);
      if (!recorderStartAccepted) {
        if (recorder_->ownedBy(recorderOwner) ||
            recorder_->operationActive() ||
            recorder_->terminalResult().pending()) {
          linkOwnedRecording_ = true;
          (void)requestLinkRecordingStop(requestId, false, true);
        } else if (acquired) {
          captureRouter_->release(AudioCaptureOwner::localCapsule);
          transactionGate_.reset();
          sendError(requestId, "recording start failed");
        }
        if (!acquired) sendBusy(requestId);
      } else {
        linkOwnedRecording_ = true;
        if (!linkRecordingStart_.begin(requestId, sessionId)) {
          (void)requestLinkRecordingStop(requestId, false, true);
        } else {
          linkRecordingCapsuleId_ = id;
        }
      }
    }
  } else if (strcmp(operation, "stop") == 0) {
    if (!recorder_->recording() || !linkOwnedRecording_) {
      sendError(requestId, "recording is not active");
    }
    else {
      (void)requestLinkRecordingStop(requestId, true, true);
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
  const String resultPath = String(kCapsuleSystem) + "/commands/results/" +
      transactionId + ".json";
  const bool alreadyComplete = storageExists(resultPath, StorageAccess::read);
  if (mutationRecoveryBlocked_ && !alreadyComplete) {
    commandStorageActive_ = false;
    commandStorageReservation_.release();
    sendBusy(requestId, 1000);
    return;
  }
  if (beginCommandLoad(requestId, path, transactionId, alreadyComplete)) return;
  commandStorageActive_ = false;
  commandStorageReservation_.release();
  sendError(requestId, "command load failed");
  rememberCompleted(requestId);
  if (activeMaintenance_.isEmpty()) releaseRequestLease();
}

bool PokePodLinkService::beginCommandLoad(
    uint32_t requestId, const String &path, const String &transactionId,
    bool alreadyComplete) {
  if (fs_ == nullptr || commandLoadState_ != CommandLoadState::none) {
    return false;
  }
  StorageIoLease lease = StorageCoordinator::instance().acquireIo(
      storageOwner(), StorageAccess::read, storageIoTimeout());
  if (!lease) return false;
  File file = fs_->open(path, FILE_READ);
  const size_t bytes = file ? file.size() : 0;
  if (!file || file.isDirectory() || bytes == 0 ||
      bytes > kMaxCommandJsonBytes ||
      !commandLoadValue_.reserve(bytes + 1)) {
    if (file) file.close();
    return false;
  }
  commandLoadFile_ = file;
  commandLoadPath_ = path;
  commandLoadTransactionId_ = transactionId;
  commandLoadRequestId_ = requestId;
  commandLoadExpected_ = static_cast<uint32_t>(bytes);
  commandLoadRespond_ = sessionActive_ && transferPermitted();
  commandLoadAlreadyComplete_ = alreadyComplete;
  commandLoadState_ = CommandLoadState::reading;
  return true;
}

void PokePodLinkService::advanceCommandLoad() {
  if (commandLoadState_ == CommandLoadState::none) return;
  if (commandLoadState_ == CommandLoadState::dispatch) {
    dispatchLoadedCommand();
    return;
  }
  const bool permitted = commandLoadRespond_ && sessionActive_ &&
      transferPermitted();
  StorageIoLease lease = StorageCoordinator::instance().acquireIo(
      storageOwner(), StorageAccess::read, 0);
  if (!lease) return;
  if (!permitted || !commandLoadFile_) {
    if (commandLoadFile_) commandLoadFile_.close();
    lease.release();
    finishCommandLoad(true);
    return;
  }
  if (payload_ == nullptr) {
    commandLoadFile_.close();
    lease.release();
    finishCommandLoad(true);
    return;
  }
  const int available = commandLoadFile_.available();
  if (available <= 0) {
    commandLoadFile_.close();
    if (commandLoadValue_.length() != commandLoadExpected_ ||
        !transferPermitted()) {
      lease.release();
      finishCommandLoad(true);
      return;
    }
    commandLoadState_ = CommandLoadState::dispatch;
    return;
  }
  const size_t wanted = std::min<size_t>(
      kCommandReadBytesPerPoll, static_cast<size_t>(available));
  const int received = commandLoadFile_.read(payload_, wanted);
  if (received <= 0 || !commandLoadValue_.concat(
          reinterpret_cast<const char *>(payload_),
          static_cast<unsigned int>(received))) {
    commandLoadFile_.close();
    lease.release();
    finishCommandLoad(true);
    return;
  }
  if (!transferPermitted()) commandLoadRespond_ = false;
}

void PokePodLinkService::finishCommandLoad(bool keepCommand) {
  if (!keepCommand && !commandLoadPath_.isEmpty()) {
    queueDeferredTreeCleanup(commandLoadPath_);
  }
  commandLoadFile_ = File();
  commandLoadValue_ = "";
  commandLoadPath_ = "";
  commandLoadTransactionId_ = "";
  commandLoadRequestId_ = 0;
  commandLoadExpected_ = 0;
  commandLoadRespond_ = false;
  commandLoadAlreadyComplete_ = false;
  commandLoadState_ = CommandLoadState::none;
  commandStorageActive_ = false;
  commandStorageReservation_.release();
}

void PokePodLinkService::dispatchLoadedCommand() {
  if (!commandLoadRespond_ || !sessionActive_ || !transferPermitted()) {
    finishCommandLoad(true);
    return;
  }
  cJSON *root = cJSON_ParseWithLength(
      commandLoadValue_.c_str(), commandLoadValue_.length());
  if (!transferPermitted()) {
    cJSON_Delete(root);
    finishCommandLoad(true);
    return;
  }
  const uint32_t requestId = commandLoadRequestId_;
  const String path = commandLoadPath_;
  const String transactionId = commandLoadTransactionId_;
  if (commandLoadAlreadyComplete_) {
    applyCompletedCommandSideEffects(root);
    cJSON_Delete(root);
    const uint32_t cleanupRequest = commandLoadRespond_ && sessionActive_ &&
        transferPermitted() ? requestId : 0;
    queueDeferredTreeCleanup(path);
    commandCleanupPending_ = true;
    commandCleanupRequestId_ = cleanupRequest;
    commandLoadFile_ = File();
    commandLoadValue_ = "";
    commandLoadPath_ = "";
    commandLoadTransactionId_ = "";
    commandLoadRequestId_ = 0;
    commandLoadExpected_ = 0;
    commandLoadRespond_ = false;
    commandLoadAlreadyComplete_ = false;
    commandLoadState_ = CommandLoadState::none;
    return;
  }
  const BatchStart batch = tryStartBatchCommand(
      requestId, root, path, transactionId);
  if (batch == BatchStart::started) {
    commandLoadFile_ = File();
    commandLoadValue_ = "";
    commandLoadPath_ = "";
    commandLoadTransactionId_ = "";
    commandLoadRequestId_ = 0;
    commandLoadExpected_ = 0;
    commandLoadRespond_ = false;
    commandLoadAlreadyComplete_ = false;
    commandLoadState_ = CommandLoadState::none;
    return;
  }
  if (batch == BatchStart::rejected) {
    cJSON_Delete(root);
    finishCommandLoad(true);
    return;
  }
  if (tryStartTextCommand(requestId, root, path, transactionId)) {
    cJSON_Delete(root);
    commandLoadFile_ = File();
    commandLoadValue_ = "";
    commandLoadPath_ = "";
    commandLoadTransactionId_ = "";
    commandLoadRequestId_ = 0;
    commandLoadExpected_ = 0;
    commandLoadRespond_ = false;
    commandLoadAlreadyComplete_ = false;
    commandLoadState_ = CommandLoadState::none;
    return;
  }
  const bool validEnvelope = root != nullptr && cJSON_IsObject(root) &&
      jsonInt64(root, "schemaVersion") == 2 &&
      sameUuid(jsonString(root, "transactionId"), transactionId.c_str());
  const char *message = validEnvelope
      ? "command operation is not supported by this firmware"
      : "command JSON is malformed";
  const bool accepted = startCommandFailureResult(
      requestId, path, transactionId, message);
  cJSON_Delete(root);
  if (!accepted && log_ != nullptr) {
    log_->printf(
        "{\"event\":\"link_command_failed\",\"transaction\":\"%s\","
        "\"message\":\"%s\"}\n",
        transactionId.c_str(), message);
  }
  commandLoadFile_ = File();
  commandLoadValue_ = "";
  commandLoadPath_ = "";
  commandLoadTransactionId_ = "";
  commandLoadRequestId_ = 0;
  commandLoadExpected_ = 0;
  commandLoadRespond_ = false;
  commandLoadAlreadyComplete_ = false;
  commandLoadState_ = CommandLoadState::none;
  if (!accepted) {
    commandStorageActive_ = false;
    commandStorageReservation_.release();
  }
}

bool PokePodLinkService::tryStartTextCommand(
    uint32_t requestId, void *jsonRoot, const String &path,
    const String &transactionId) {
  cJSON *root = asJson(jsonRoot);
  const char *operation = root == nullptr ? nullptr :
      jsonString(root, "operation");
  const bool correction = operation != nullptr &&
      strcmp(operation, "commitCorrection") == 0;
  const bool finalText = operation != nullptr &&
      strcmp(operation, "commitFinalText") == 0;
  if (!correction && !finalText) {
    return false;
  }
  cJSON *ids = cJSON_GetObjectItemCaseSensitive(root, "capsuleIds");
  const char *id = cJSON_IsArray(ids) && cJSON_GetArraySize(ids) == 1
      ? cJSON_GetStringValue(cJSON_GetArrayItem(ids, 0)) : nullptr;
  const char *maintenanceId = jsonString(root, "maintenanceId");
  const char *stagedPath = jsonString(root, "stagedPath");
  const CapsuleSummary *record = isUuid(id) ? library_->find(id) : nullptr;
  const String expectedStaged = ".staging/" + transactionId + "/" +
      (id == nullptr ? "" : id);
  const String stagedDirectory = String(kCapsuleRoot) + "/" + expectedStaged;
  const String prepared = stagedDirectory +
      (correction ? "/polished.md" : "/final.md");
  if (jsonInt64(root, "schemaVersion") != 2 ||
      !sameUuid(jsonString(root, "transactionId"), transactionId.c_str()) ||
      record == nullptr || activeMaintenance_.isEmpty() ||
      !isUuid(maintenanceId) ||
      !activeMaintenance_.equalsIgnoreCase(maintenanceId)) {
    return false;
  }

  const int expectedRevision = static_cast<int>(
      jsonInt64(root, "expectedRevision"));
  String metadataTarget;
  cJSON *metadata = nullptr;
  if (correction) {
    if (stagedPath == nullptr || String(stagedPath) != expectedStaged ||
        !storageExists(prepared, StorageAccess::read)) {
      return false;
    }
    metadataTarget = record->directory + "/processing.json";
    metadata = cJSON_Parse(readText(metadataTarget, 8192).c_str());
  } else {
    metadataTarget = record->directory + "/capsule.json";
    metadata = cJSON_Parse(readText(metadataTarget, 8192).c_str());
  }
  cJSON *revision = metadata == nullptr ? nullptr :
      cJSON_GetObjectItemCaseSensitive(metadata, "revision");
  if (!cJSON_IsNumber(revision) || revision->valueint != expectedRevision) {
    cJSON_Delete(metadata);
    return false;
  }
  cJSON_SetNumberValue(revision, expectedRevision + 1);
  if (correction) {
    cJSON_ReplaceItemInObjectCaseSensitive(
        metadata, "status", cJSON_CreateString("ready"));
    replaceStringOrNull(metadata, "polishedTextFile", "polished.md");
    replaceStringOrNull(metadata, "errorStage", nullptr);
    replaceStringOrNull(metadata, "error", nullptr);
  } else {
    replaceStringOrNull(metadata, "updatedAt", board_->utcNow().c_str());
  }
  commandTextMetadataValue_ = printed(metadata) + "\n";
  cJSON_Delete(metadata);

  CapsuleTransactionInput inputs[2];
  const String textTarget = record->directory +
      (correction ? "/polished.md" : "/final.md");
  if (stagedPath != nullptr && String(stagedPath) == expectedStaged &&
      storageExists(prepared, StorageAccess::read)) {
    inputs[0] = {textTarget, nullptr, prepared};
  } else if (finalText && jsonString(root, "finalText") != nullptr &&
             strlen(jsonString(root, "finalText")) <= 8192U) {
    commandTextValue_ = jsonString(root, "finalText");
    commandTextSource_.bind(commandTextValue_);
    inputs[0] = {textTarget, &commandTextSource_, String()};
  } else {
    return false;
  }
  commandTextMetadataSource_.bind(commandTextMetadataValue_);
  inputs[1] = {metadataTarget, &commandTextMetadataSource_, String()};
  const bool hadStagedText = stagedPath != nullptr &&
      String(stagedPath) == expectedStaged;
  const bool started = transactionRunner_.startCommit(
      transactionId.c_str(), inputs, 2, storageOwner());
  if (!started) return false;
  transactionGate_.beginOperation(transferGate_);
  transactionPurpose_ = TransactionPurpose::commandText;
  commandTextRequestId_ = requestId;
  commandTextPath_ = path;
  commandTextTransactionId_ = transactionId;
  commandTextStagingDirectory_ = hadStagedText ? stagedDirectory : String();
  commandTextRespond_ = sessionActive_ && transferPermitted();
  return true;
}

void PokePodLinkService::finishTextCommand(bool committed) {
  cJSON *result = cJSON_CreateObject();
  cJSON_AddNumberToObject(result, "schemaVersion", 1);
  cJSON_AddStringToObject(result, "commandId",
                          commandTextTransactionId_.c_str());
  cJSON_AddStringToObject(result, "transactionId",
                          commandTextTransactionId_.c_str());
  cJSON_AddBoolToObject(result, "ok", committed);
  cJSON_AddBoolToObject(result, "success", committed);
  cJSON_AddStringToObject(result, "message",
                          committed ? "committed" : "text commit failed");
  cJSON_AddStringToObject(result, "completedAt", board_->utcNow().c_str());
  const String resultPath = String(kCapsuleSystem) + "/commands/results/" +
      commandTextTransactionId_ + ".json";
  commandTextMetadataValue_ = printed(result) + "\n";
  cJSON_Delete(result);
  transactionPurpose_ = TransactionPurpose::none;
  transactionGate_.reset();
  commandTextMetadataSource_.bind(commandTextMetadataValue_);
  const CapsuleTransactionInput input{
      resultPath, &commandTextMetadataSource_, String()};
  const String key = commandTextTransactionId_ + "-result";
  if (!transactionRunner_.startCommit(key.c_str(), &input, 1,
                                      storageOwner())) {
    finishTextResult(false);
    return;
  }
  // Result durability is local recovery work.  A Wi-Fi deadline or USB
  // disconnect suppresses the response, but it must not leave a published
  // text mutation without its idempotency result.
  transactionPurpose_ = TransactionPurpose::commandTextResult;
}

void PokePodLinkService::finishTextResult(bool persisted) {
  const uint32_t requestId = commandTextRequestId_;
  const bool respond = persisted && commandTextRespond_ && sessionActive_ &&
      transferPermitted();
  transactionPurpose_ = TransactionPurpose::none;
  if (persisted) {
    if (!commandTextStagingDirectory_.isEmpty()) {
      queueDeferredTreeCleanup(commandTextStagingDirectory_);
    }
    queueDeferredTreeCleanup(commandTextPath_);
    commandCleanupPending_ = true;
    commandCleanupRequestId_ = respond ? requestId : 0;
  } else {
    // Keep the command and staged input as replay evidence.  No product
    // mutation is acknowledged without the durable result file.
    commandStorageActive_ = false;
    commandStorageReservation_.release();
  }
  commandTextRequestId_ = 0;
  commandTextPath_ = "";
  commandTextTransactionId_ = "";
  commandTextStagingDirectory_ = "";
  commandTextValue_ = "";
  commandTextMetadataValue_ = "";
  commandTextRespond_ = false;
  (void)library_->requestScan();
}

bool PokePodLinkService::startCommandFailureResult(
    uint32_t requestId, const String &path, const String &transactionId,
    const char *message) {
  cJSON *result = cJSON_CreateObject();
  cJSON_AddNumberToObject(result, "schemaVersion", 1);
  cJSON_AddStringToObject(result, "commandId", transactionId.c_str());
  cJSON_AddStringToObject(result, "transactionId", transactionId.c_str());
  cJSON_AddBoolToObject(result, "ok", false);
  cJSON_AddBoolToObject(result, "success", false);
  cJSON_AddStringToObject(result, "message",
                          message == nullptr ? "command failed" : message);
  cJSON_AddStringToObject(result, "completedAt", board_->utcNow().c_str());
  commandTextMetadataValue_ = printed(result) + "\n";
  cJSON_Delete(result);
  const String resultPath = String(kCapsuleSystem) + "/commands/results/" +
      transactionId + ".json";
  commandTextMetadataSource_.bind(commandTextMetadataValue_);
  const CapsuleTransactionInput input{
      resultPath, &commandTextMetadataSource_, String()};
  const String key = transactionId + "-result";
  if (!transactionRunner_.startCommit(key.c_str(), &input, 1,
                                      storageOwner())) return false;
  transactionPurpose_ = TransactionPurpose::commandFailureResult;
  commandTextRequestId_ = requestId;
  commandTextPath_ = path;
  commandTextTransactionId_ = transactionId;
  commandTextRespond_ = sessionActive_ && transferPermitted();
  return true;
}

void PokePodLinkService::finishCommandFailureResult(bool persisted) {
  const uint32_t requestId = commandTextRequestId_;
  const bool respond = persisted && commandTextRespond_ && sessionActive_ &&
      transferPermitted();
  transactionPurpose_ = TransactionPurpose::none;
  if (persisted) {
    queueDeferredTreeCleanup(commandTextPath_);
    commandCleanupPending_ = true;
    commandCleanupRequestId_ = respond ? requestId : 0;
  } else {
    commandStorageActive_ = false;
    commandStorageReservation_.release();
  }
  commandTextRequestId_ = 0;
  commandTextPath_ = "";
  commandTextTransactionId_ = "";
  commandTextMetadataValue_ = "";
  commandTextRespond_ = false;
}

PokePodLinkService::BatchStart PokePodLinkService::tryStartBatchCommand(
    uint32_t requestId, void *jsonRoot, const String &path,
    const String &transactionId) {
  cJSON *root = asJson(jsonRoot);
  const char *operation = root == nullptr ? nullptr :
      jsonString(root, "operation");
  if (root == nullptr || !cJSON_IsObject(root) ||
      operation == nullptr || !batchOperation(operation)) {
    return BatchStart::notApplicable;
  }
  cJSON *ids = cJSON_GetObjectItemCaseSensitive(root, "capsuleIds");
  const bool simple = capsuleBatchSimpleOperation(operation);
  const int count = simple ? 1 :
      (cJSON_IsArray(ids) ? cJSON_GetArraySize(ids) : -1);
  const char *maintenanceId = jsonString(root, "maintenanceId");
  const bool deleteFolder = strcmp(operation, "deleteFolderToInbox") == 0;
  const char *finalizeFolder = deleteFolder
      ? jsonString(root, "folderPath") : nullptr;
  const char *rejection = nullptr;
  if (jsonInt64(root, "schemaVersion") != 2 ||
      !sameUuid(jsonString(root, "transactionId"), transactionId.c_str())) {
    rejection = "invalid batch command";
  } else if (count < 0 ||
             count > static_cast<int>(LinkCommandExecutor::kMaximumItems)) {
    rejection = "invalid batch size";
  } else if (strcmp(operation, "beginMaintenance") == 0 &&
             (!isUuid(maintenanceId) ||
              (!activeMaintenance_.isEmpty() &&
               !activeMaintenance_.equalsIgnoreCase(maintenanceId)))) {
    rejection = "another maintenance session is active";
  } else if (strcmp(operation, "beginMaintenance") != 0 &&
             strcmp(operation, "rescan") != 0 &&
             (activeMaintenance_.isEmpty() || !isUuid(maintenanceId) ||
              !activeMaintenance_.equalsIgnoreCase(maintenanceId))) {
    rejection = "maintenance session does not own the device";
  } else if (deleteFolder && !safeFolder(finalizeFolder, false)) {
    rejection = "invalid folder delete";
  } else if (!simple && batchSeenIds_ == nullptr &&
             (batchSeenIds_ = static_cast<uint8_t *>(heap_caps_calloc(
                  1024, 17, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT))) == nullptr) {
    rejection = "batch identity table unavailable";
  } else if (!batchJournalStore_.create(
                 transactionId, operation, static_cast<uint16_t>(count),
                 storageOwner(), batchJournalState_, finalizeFolder)) {
    rejection = "batch journal unavailable";
  }
  // A recognized command never falls through to the synchronous legacy path.
  // Validation/journal failures are represented as an empty failed executor;
  // it persists the normal durable command result before acknowledging upload.
  const bool executorStarted = rejection == nullptr
      ? batchExecutor_.begin(static_cast<size_t>(count), deleteFolder)
      : batchExecutor_.beginRejected();
  if (!executorStarted) {
    return BatchStart::rejected;
  }
  batchJsonRoot_ = root;
  batchRequestId_ = requestId;
  batchCommandPath_ = path;
  batchTransactionId_ = transactionId;
  batchMessage_ = rejection == nullptr ? "committed" : rejection;
  batchPending_ = BatchPending::none;
  batchPendingStep_ = 0;
  batchNextIdItem_ = cJSON_IsArray(ids) ? ids->child : nullptr;
  batchNextIdIndex_ = 0;
  if (!simple && batchSeenIds_ != nullptr) memset(batchSeenIds_, 0, 1024 * 17);
  transactionGate_.beginOperation(transferGate_);
  log_->printf(
      "{\"event\":\"link_batch_begin\",\"transaction\":\"%s\","
      "\"operation\":\"%s\",\"count\":%u}\n",
      transactionId.c_str(), operation, static_cast<unsigned>(count));
  return BatchStart::started;
}

void PokePodLinkService::updateBatchJournalState(bool itemInFlight) {
  switch (batchExecutor_.phase()) {
    case LinkCommandExecutor::Phase::preflight:
      batchJournalState_.phase = CapsuleBatchPhase::preflight;
      break;
    case LinkCommandExecutor::Phase::apply:
    case LinkCommandExecutor::Phase::finalize:
      batchJournalState_.phase = CapsuleBatchPhase::apply;
      break;
    case LinkCommandExecutor::Phase::rollback:
      batchJournalState_.phase = CapsuleBatchPhase::rollback;
      break;
    case LinkCommandExecutor::Phase::result:
      batchJournalState_.phase = CapsuleBatchPhase::result;
      break;
    case LinkCommandExecutor::Phase::cleanup:
    case LinkCommandExecutor::Phase::finished:
    case LinkCommandExecutor::Phase::idle:
      batchJournalState_.phase = CapsuleBatchPhase::cleanup;
      break;
  }
  batchJournalState_.cursor = static_cast<uint16_t>(batchExecutor_.cursor());
  batchJournalState_.applied = static_cast<uint16_t>(batchExecutor_.applied());
  batchJournalState_.flags = 0;
  if (batchExecutor_.success()) {
    batchJournalState_.flags |= capsuleBatchSuccess;
  }
  if (batchExecutor_.responseAllowed()) {
    batchJournalState_.flags |= capsuleBatchResponseAllowed;
  }
  if (batchExecutor_.rollbackFailed()) {
    batchJournalState_.flags |= capsuleBatchRollbackFailed;
  }
  if (batchExecutor_.finalizeApplied()) {
    batchJournalState_.flags |= capsuleBatchFinalizeApplied;
  }
  if (itemInFlight) batchJournalState_.flags |= capsuleBatchItemInFlight;
  sealCapsuleBatchState(batchJournalState_);
}

String PokePodLinkService::batchArtifactPath(size_t index,
                                              const char *suffix) const {
  return String(CapsuleBatchJournalStore::kDirectory) + "/" +
      batchTransactionId_ + "-" + String(static_cast<unsigned>(index)) +
      (suffix == nullptr ? "" : suffix);
}

String PokePodLinkService::batchPurgePath(
    const StoredCapsuleBatchPlan &plan) const {
  return String(kCapsuleStaging) + "/purge-batch-" +
      batchTransactionId_ + "-" + plan.id;
}

String PokePodLinkService::batchFolderStagingPath() const {
  return String(kCapsuleStaging) + "/folder-batch-" + batchTransactionId_;
}

void PokePodLinkService::advanceBatchCommand() {
  if (!batchExecutor_.active()) return;
  if (batchPending_ != BatchPending::none) {
    advanceBatchPending();
    return;
  }
  const LinkCommandExecutor::Work work =
      batchExecutor_.poll(batchForegroundPermitted());
  if (work.action == LinkCommandExecutor::Action::none) return;
  batchWork_ = work;
  if (!startBatchWork(work)) finishBatchWork(false);
}

bool PokePodLinkService::startBatchWork(
    const LinkCommandExecutor::Work &work) {
  switch (work.action) {
    case LinkCommandExecutor::Action::checkpoint:
      updateBatchJournalState(batchExecutor_.checkpointMarksItemInFlight());
      finishBatchWork(batchJournalStore_.checkpoint(
          batchJournalState_, storageOwner()));
      return true;
    case LinkCommandExecutor::Action::preflightItem:
      return startBatchPreflight(work.index);
    case LinkCommandExecutor::Action::applyItem:
      return startBatchApply(work.index);
    case LinkCommandExecutor::Action::finalizeApply:
      return startBatchFinalize();
    case LinkCommandExecutor::Action::rollbackFinalize:
      return startBatchRollbackFinalize();
    case LinkCommandExecutor::Action::rollbackItem:
      return startBatchRollback(work.index);
    case LinkCommandExecutor::Action::persistResult:
      return startBatchResultPersistence();
    case LinkCommandExecutor::Action::cleanup:
      if (!cleanupBatchArtifacts()) {
        finishBatchWork(false);
      } else if (batchPending_ == BatchPending::none) {
        finishBatchWork(true);
      }
      return true;
    case LinkCommandExecutor::Action::finish:
      finishBatchCommand();
      finishBatchWork(true);
      return true;
    case LinkCommandExecutor::Action::none:
      return true;
  }
  return false;
}

void PokePodLinkService::finishBatchWork(bool ok) {
  batchPending_ = BatchPending::none;
  batchPendingStep_ = 0;
  batchExecutor_.completeStep(ok);
}

bool PokePodLinkService::rememberBatchId(const char *uuid) {
  if (batchSeenIds_ == nullptr || !isUuid(uuid)) return false;
  uint8_t value[16] = {};
  size_t byte = 0;
  uint8_t high = 0;
  bool haveHigh = false;
  for (const char *cursor = uuid; *cursor != '\0'; ++cursor) {
    if (*cursor == '-') continue;
    const char c = *cursor;
    const uint8_t nibble = c >= '0' && c <= '9' ? c - '0' :
        c >= 'a' && c <= 'f' ? c - 'a' + 10 : c - 'A' + 10;
    if (!haveHigh) {
      high = static_cast<uint8_t>(nibble << 4);
      haveHigh = true;
    } else {
      if (byte >= sizeof(value)) return false;
      value[byte++] = static_cast<uint8_t>(high | nibble);
      haveHigh = false;
    }
  }
  if (byte != sizeof(value) || haveHigh) return false;
  uint32_t hash = 2166136261U;
  for (const uint8_t octet : value) hash = (hash ^ octet) * 16777619U;
  for (size_t probe = 0; probe < 1024; ++probe) {
    uint8_t *slot = batchSeenIds_ + (((hash + probe) & 1023U) * 17U);
    if (slot[0] == 0) {
      slot[0] = 1;
      memcpy(slot + 1, value, sizeof(value));
      return true;
    }
    if (memcmp(slot + 1, value, sizeof(value)) == 0) return false;
  }
  return false;
}

bool PokePodLinkService::buildBatchPlan(
    size_t index, StoredCapsuleBatchPlan &plan, String &message) {
  cJSON *root = asJson(batchJsonRoot_);
  const char *operation = root == nullptr ? nullptr :
      jsonString(root, "operation");
  if (capsuleBatchSimpleOperation(operation)) {
    return index == 0 && buildSimpleBatchPlan(root, plan, message);
  }
  cJSON *item = index == batchNextIdIndex_ ?
      asJson(batchNextIdItem_) : nullptr;
  const char *rawId = cJSON_GetStringValue(item);
  if (!isUuid(rawId)) {
    message = "invalid capsule id";
    return false;
  }
  if (!rememberBatchId(rawId)) {
    message = "duplicate capsule id";
    return false;
  }
  batchNextIdItem_ = item->next;
  ++batchNextIdIndex_;
  String id(rawId);
  id.toLowerCase();

  operation = batchJournalState_.operation;
  const bool restore = strcmp(operation, "restoreCapsules") == 0;
  const bool purge = strcmp(operation, "purgeCapsules") == 0;
  const bool remove = strcmp(operation, "deleteCapsules") == 0;
  const bool copy = strcmp(operation, "copyCapsules") == 0;
  const bool move = strcmp(operation, "moveCapsules") == 0;
  const bool evacuate = strcmp(operation, "deleteFolderToInbox") == 0;
  const bool metadata = strcmp(operation, "setFavorite") == 0 ||
      strcmp(operation, "addTags") == 0 ||
      strcmp(operation, "removeTags") == 0 ||
      strcmp(operation, "renameTag") == 0 ||
      strcmp(operation, "mergeTag") == 0 ||
      strcmp(operation, "deleteTag") == 0;

  String source;
  String target;
  String targetId = id;
  if (restore || purge) {
    source = String(kCapsuleTrash) + "/" + id;
  } else if (evacuate) {
    const char *folder = jsonString(root, "folderPath");
    if (!safeFolder(folder, false)) {
      message = "invalid user folder";
      return false;
    }
    const String folderRoot = String(kCapsuleRoot) + "/" + folder + "/";
    source = activeCapsuleDirectory(id);
    if (!source.startsWith(folderRoot)) {
      message = "folder contents changed during preflight";
      return false;
    }
  } else {
    source = activeCapsuleDirectory(id);
  }
  if (source.isEmpty() || !storageExists(source, StorageAccess::read)) {
    message = "capsule is missing";
    return false;
  }

  if (move || copy) {
    const String folder = folderDirectory(jsonString(root, "destination"));
    if (folder.isEmpty() || !storageExists(folder, StorageAccess::read)) {
      message = "invalid move destination";
      return false;
    }
    if (copy) targetId = newUuid();
    target = folder + "/" + targetId;
  } else if (remove) {
    target = String(kCapsuleTrash) + "/" + id;
  } else if (restore) {
    cJSON *trash = cJSON_Parse(readText(source + "/trash.json", 8192).c_str());
    const char *folderName = trash == nullptr ? nullptr :
        jsonString(trash, "originalFolder");
    String folder = folderDirectory(folderName);
    cJSON_Delete(trash);
    if (folder.isEmpty() || !storageExists(folder, StorageAccess::read)) {
      folder = kCapsuleInbox;
    }
    target = folder + "/" + id;
  } else if (evacuate) {
    target = String(kCapsuleInbox) + "/" + id;
  }
  const bool noOp = move && source == target;
  if (!target.isEmpty() && !noOp &&
      storageExists(target, StorageAccess::read)) {
    message = "capsule target already exists";
    return false;
  }

  cJSON *expected = cJSON_GetObjectItemCaseSensitive(
      root, "expectedRevisions");
  cJSON *wanted = cJSON_IsObject(expected) ?
      cJSON_GetObjectItemCaseSensitive(expected, id.c_str()) : nullptr;
  const String revisionPath = source +
      (restore || purge ? "/trash.json" : "/capsule.json");
  cJSON *revisionRoot = cJSON_Parse(readText(revisionPath, 8192).c_str());
  cJSON *revision = revisionRoot == nullptr ? nullptr :
      cJSON_GetObjectItemCaseSensitive(revisionRoot, "revision");
  const bool revisionMatches = cJSON_IsNumber(wanted) &&
      cJSON_IsNumber(revision) && wanted->valueint == revision->valueint;
  const int revisionValue = cJSON_IsNumber(revision) ? revision->valueint : -1;
  cJSON_Delete(revisionRoot);
  if (!revisionMatches) {
    message = "revision conflict for " + id;
    return false;
  }

  memset(&plan, 0, sizeof(plan));
  strlcpy(plan.id, id.c_str(), sizeof(plan.id));
  strlcpy(plan.targetId, targetId.c_str(), sizeof(plan.targetId));
  strlcpy(plan.source, source.c_str(), sizeof(plan.source));
  if (!target.isEmpty()) strlcpy(plan.target, target.c_str(), sizeof(plan.target));
  plan.expectedRevision = revisionValue;
  if (noOp) plan.flags |= kBatchPlanNoOp;
  if ((move || restore || evacuate || metadata) && !noOp) {
    plan.flags |= kBatchPlanCapsuleBackup;
  }
  if (restore) plan.flags |= kBatchPlanTrashBackup;
  sealCapsuleBatchPlan(plan);
  if (!validCapsuleBatchPlan(plan, operation)) {
    message = "batch plan is invalid";
    return false;
  }
  return true;
}

bool PokePodLinkService::buildSimpleBatchPlan(
    void *jsonRoot, StoredCapsuleBatchPlan &plan, String &message) {
  cJSON *root = asJson(jsonRoot);
  const char *operation = root == nullptr ? nullptr :
      jsonString(root, "operation");
  if (!capsuleBatchSimpleOperation(operation)) return false;
  memset(&plan, 0, sizeof(plan));
  strlcpy(plan.id, batchTransactionId_.c_str(), sizeof(plan.id));

  const char *maintenanceId = jsonString(root, "maintenanceId");
  if (strcmp(operation, "rescan") == 0) {
    strlcpy(plan.targetId, batchTransactionId_.c_str(),
            sizeof(plan.targetId));
  } else if (strcmp(operation, "beginMaintenance") == 0 ||
      strcmp(operation, "endMaintenance") == 0) {
    const bool begin = operation[0] == 'b';
    if (!isUuid(maintenanceId) ||
        (begin && !activeMaintenance_.isEmpty() &&
         !activeMaintenance_.equalsIgnoreCase(maintenanceId)) ||
        (!begin && (activeMaintenance_.isEmpty() ||
         !activeMaintenance_.equalsIgnoreCase(maintenanceId)))) {
      message = begin ? "another maintenance session is active" :
                        "maintenance session does not own the device";
      return false;
    }
    if (!activeMaintenance_.isEmpty()) {
      strlcpy(plan.source, activeMaintenance_.c_str(), sizeof(plan.source));
    }
    strlcpy(plan.targetId, maintenanceId, sizeof(plan.targetId));
  } else if (strcmp(operation, "createFolder") == 0 ||
             strcmp(operation, "renameFolder") == 0) {
    const char *folder = jsonString(root, "folderPath");
    if (!safeFolder(folder, false)) {
      message = "invalid user folder";
      return false;
    }
    const String source = String(kCapsuleRoot) + "/" + folder;
    const bool rename = operation[0] == 'r';
    if ((!rename && storageExists(source, StorageAccess::read)) ||
        (rename && !storageExists(source, StorageAccess::read))) {
      message = rename ? "invalid folder rename" : "folder already exists";
      return false;
    }
    strlcpy(plan.source, source.c_str(), sizeof(plan.source));
    if (rename) {
      const char *newFolder = jsonString(root, "newFolderPath");
      if (!safeFolder(newFolder, false)) {
        message = "invalid folder rename";
        return false;
      }
      const String target = String(kCapsuleRoot) + "/" + newFolder;
      if (storageExists(target, StorageAccess::read) ||
          !storageExists(parentPath(target), StorageAccess::read)) {
        message = "folder rename target is invalid";
        return false;
      }
      strlcpy(plan.target, target.c_str(), sizeof(plan.target));
    } else if (!storageExists(parentPath(source), StorageAccess::read)) {
      message = "folder parent is missing";
      return false;
    }
  } else {
    cJSON *ids = cJSON_GetObjectItemCaseSensitive(root, "capsuleIds");
    const char *id = cJSON_IsArray(ids) && cJSON_GetArraySize(ids) == 1
        ? cJSON_GetStringValue(cJSON_GetArrayItem(ids, 0)) : nullptr;
    if (!isUuid(id)) {
      message = "single capsule id is required";
      return false;
    }
    strlcpy(plan.id, id, sizeof(plan.id));
    strlcpy(plan.targetId, id, sizeof(plan.targetId));
    if (strcmp(operation, "requeueTranscription") == 0) {
      const CapsuleSummary *record = library_->find(id);
      cJSON *expected = cJSON_GetObjectItemCaseSensitive(
          root, "expectedRevisions");
      cJSON *wanted = cJSON_IsObject(expected) ?
          cJSON_GetObjectItemCaseSensitive(expected, id) : nullptr;
      if (record == nullptr || record->readOnly || !cJSON_IsNumber(wanted) ||
          wanted->valueint != record->processingRevision ||
          record->status != CapsuleStatus::failed) {
        message = "processing revision conflict";
        return false;
      }
      strlcpy(plan.source, record->directory.c_str(), sizeof(plan.source));
      plan.expectedRevision = record->processingRevision;
      plan.flags |= kBatchPlanProcessingBackup;
    } else {
      const char *stagedPath = jsonString(root, "stagedPath");
      const char *destination = jsonString(root, "destination");
      const String expected = ".staging/" + batchTransactionId_ + "/" + id;
      const String source = String(kCapsuleRoot) + "/" +
          (stagedPath == nullptr ? "" : stagedPath);
      const String targetFolder = folderDirectory(destination);
      const String target = targetFolder + "/" + id;
      const String capsuleText = readText(source + "/capsule.json", 8192);
      const String processingText = readText(
          source + "/processing.json", 8192);
      cJSON *capsule = cJSON_Parse(capsuleText.c_str());
      cJSON *processing = cJSON_Parse(processingText.c_str());
      const char *metadataId = capsule == nullptr ? nullptr :
          jsonString(capsule, "id");
      const char *processingId = processing == nullptr ? nullptr :
          jsonString(processing, "capsuleId");
      const char *audioFile = processing == nullptr ? nullptr :
          jsonString(processing, "audioFile");
      const int64_t schema = processing == nullptr ? -1 :
          jsonInt64(processing, "schemaVersion");
      const bool valid = stagedPath != nullptr &&
          String(stagedPath) == expected && !targetFolder.isEmpty() &&
          sameUuid(id, metadataId) && sameUuid(id, processingId) &&
          (schema == 1 || schema == 2) && safeCapsuleFileName(audioFile) &&
          storageExists(source + "/" + audioFile, StorageAccess::read) &&
          storageExists(targetFolder, StorageAccess::read) &&
          !storageExists(target, StorageAccess::read);
      cJSON_Delete(capsule);
      cJSON_Delete(processing);
      if (!valid) {
        message = "invalid staged import";
        return false;
      }
      strlcpy(plan.source, source.c_str(), sizeof(plan.source));
      strlcpy(plan.target, target.c_str(), sizeof(plan.target));
    }
  }
  sealCapsuleBatchPlan(plan);
  if (!validCapsuleBatchPlan(plan, operation)) {
    message = "command plan is invalid";
    return false;
  }
  return true;
}

bool PokePodLinkService::startBatchPreflight(size_t index) {
  String message;
  if (!buildBatchPlan(index, batchPlan_, message) ||
      !batchJournalStore_.writePlan(batchJournalState_,
                                    static_cast<uint16_t>(index), batchPlan_,
                                    storageOwner())) {
    batchMessage_ = message.isEmpty() ? "batch preflight failed" : message;
    return false;
  }
  if ((batchPlan_.flags & kBatchPlanCapsuleBackup) != 0) {
    const bool started = batchTreeStepper_.beginCopy(
        *fs_, String(batchPlan_.source) + "/capsule.json",
        batchArtifactPath(index, ".capsule.bak"), storageOwner(),
        [](void *context) {
          return static_cast<PokePodLinkService *>(context)
              ->batchForegroundPermitted();
        }, this);
    if (!started) return false;
    batchPending_ = BatchPending::backupCapsule;
    return true;
  }
  if ((batchPlan_.flags & kBatchPlanTrashBackup) != 0) {
    if (!batchTreeStepper_.beginCopy(
            *fs_, String(batchPlan_.source) + "/trash.json",
            batchArtifactPath(index, ".trash.bak"), storageOwner(),
            [](void *context) {
              return static_cast<PokePodLinkService *>(context)
                  ->batchForegroundPermitted();
            }, this)) return false;
    batchPending_ = BatchPending::backupTrash;
    return true;
  }
  if ((batchPlan_.flags & kBatchPlanProcessingBackup) != 0) {
    if (!batchTreeStepper_.beginCopy(
            *fs_, String(batchPlan_.source) + "/processing.json",
            batchArtifactPath(index, ".processing.bak"), storageOwner(),
            [](void *context) {
              return static_cast<PokePodLinkService *>(context)
                  ->batchForegroundPermitted();
            }, this)) return false;
    batchPending_ = BatchPending::backupProcessing;
    return true;
  }
  finishBatchWork(true);
  return true;
}

bool PokePodLinkService::buildBatchMetadata(
    const StoredCapsuleBatchPlan &plan, String &value, String &message) {
  cJSON *command = asJson(batchJsonRoot_);
  const char *operation = batchJournalState_.operation;
  batchMetadataSecondValue_ = "";
  if (strcmp(operation, "deleteCapsules") == 0) {
    const String source(plan.source);
    const int slash = source.lastIndexOf('/');
    const int folderStart = strlen(kCapsuleRoot) + 1;
    String folder = slash <= folderStart ? "Inbox" :
        source.substring(folderStart, slash);
    cJSON *trash = cJSON_CreateObject();
    cJSON_AddNumberToObject(trash, "schemaVersion", 1);
    cJSON_AddStringToObject(trash, "capsuleId", plan.id);
    cJSON_AddStringToObject(trash, "trashedAt", board_->utcNow().c_str());
    cJSON_AddStringToObject(trash, "originalFolder", folder.c_str());
    cJSON_AddNumberToObject(trash, "revision", plan.expectedRevision + 1);
    value = printed(trash) + "\n";
    cJSON_Delete(trash);
    return !value.isEmpty();
  }

  const bool copy = strcmp(operation, "copyCapsules") == 0;
  const String directory = copy ? String(plan.target) :
      ((strcmp(operation, "moveCapsules") == 0 ||
        strcmp(operation, "restoreCapsules") == 0 ||
        strcmp(operation, "deleteFolderToInbox") == 0)
           ? String(plan.target) : String(plan.source));
  cJSON *capsule = cJSON_Parse(
      readText(directory + "/capsule.json", 8192).c_str());
  cJSON *revision = capsule == nullptr ? nullptr :
      cJSON_GetObjectItemCaseSensitive(capsule, "revision");
  if (capsule == nullptr || !cJSON_IsNumber(revision)) {
    cJSON_Delete(capsule);
    message = "capsule metadata is malformed";
    return false;
  }
  if (copy) {
    replaceStringOrNull(capsule, "id", plan.targetId);
    replaceStringOrNull(capsule, "createdAt", board_->utcNow().c_str());
    cJSON_SetNumberValue(revision, 1);
  } else if (strcmp(operation, "setFavorite") == 0) {
    cJSON *favorite = cJSON_GetObjectItemCaseSensitive(command, "favorite");
    if (!cJSON_IsBool(favorite)) {
      cJSON_Delete(capsule);
      message = "favorite value is required";
      return false;
    }
    cJSON *replacement = cJSON_CreateBool(cJSON_IsTrue(favorite));
    if (cJSON_HasObjectItem(capsule, "favorite")) {
      cJSON_ReplaceItemInObjectCaseSensitive(capsule, "favorite", replacement);
    } else {
      cJSON_AddItemToObject(capsule, "favorite", replacement);
    }
    cJSON_SetNumberValue(revision, revision->valueint + 1);
  } else if (strcmp(operation, "addTags") == 0 ||
             strcmp(operation, "removeTags") == 0 ||
             strcmp(operation, "renameTag") == 0 ||
             strcmp(operation, "mergeTag") == 0 ||
             strcmp(operation, "deleteTag") == 0) {
    cJSON *requested = cJSON_GetObjectItemCaseSensitive(command, "tags");
    if (!cJSON_IsArray(requested) || cJSON_GetArraySize(requested) > 32) {
      cJSON_Delete(capsule);
      message = "invalid tags";
      return false;
    }
    std::vector<String> tags;
    std::vector<String> values;
    cJSON *tag = nullptr;
    cJSON_ArrayForEach(tag, requested) {
      const char *text = cJSON_GetStringValue(tag);
      if (text == nullptr || strlen(text) > 64) {
        cJSON_Delete(capsule);
        message = "invalid tags";
        return false;
      }
      tags.emplace_back(text);
    }
    cJSON *old = cJSON_GetObjectItemCaseSensitive(capsule, "tags");
    cJSON_ArrayForEach(tag, old) {
      const char *text = cJSON_GetStringValue(tag);
      if (text != nullptr) values.emplace_back(text);
    }
    if (strcmp(operation, "addTags") == 0) {
      for (const String &requestedTag : tags) {
        bool found = false;
        for (const String &existing : values) {
          if (existing.equalsIgnoreCase(requestedTag)) found = true;
        }
        if (!found) values.push_back(requestedTag);
      }
    } else if (strcmp(operation, "removeTags") == 0 ||
               strcmp(operation, "deleteTag") == 0) {
      values.erase(std::remove_if(values.begin(), values.end(),
          [&](const String &existing) {
            for (const String &requestedTag : tags) {
              if (existing.equalsIgnoreCase(requestedTag)) return true;
            }
            return false;
          }), values.end());
    } else {
      if (tags.size() != 2) {
        cJSON_Delete(capsule);
        message = "tag rename needs old and new names";
        return false;
      }
      for (String &existing : values) {
        if (existing.equalsIgnoreCase(tags[0])) existing = tags[1];
      }
      for (size_t left = 0; left < values.size(); ++left) {
        values.erase(std::remove_if(values.begin() + left + 1, values.end(),
            [&](const String &other) {
              return other.equalsIgnoreCase(values[left]);
            }), values.end());
      }
    }
    cJSON *replacement = cJSON_CreateArray();
    for (const String &existing : values) {
      cJSON_AddItemToArray(replacement,
                          cJSON_CreateString(existing.c_str()));
    }
    if (cJSON_HasObjectItem(capsule, "tags")) {
      cJSON_ReplaceItemInObjectCaseSensitive(capsule, "tags", replacement);
    } else {
      cJSON_AddItemToObject(capsule, "tags", replacement);
    }
    cJSON_SetNumberValue(revision, revision->valueint + 1);
  } else {
    cJSON_SetNumberValue(revision, revision->valueint + 1);
  }
  replaceStringOrNull(capsule, "updatedAt", board_->utcNow().c_str());
  value = printed(capsule) + "\n";
  cJSON_Delete(capsule);
  if (!copy) return !value.isEmpty();

  cJSON *processing = cJSON_Parse(
      readText(directory + "/processing.json", 8192).c_str());
  cJSON *processingRevision = processing == nullptr ? nullptr :
      cJSON_GetObjectItemCaseSensitive(processing, "revision");
  if (processing == nullptr || !cJSON_IsNumber(processingRevision)) {
    cJSON_Delete(processing);
    message = "processing metadata is malformed";
    return false;
  }
  replaceStringOrNull(processing, "capsuleId", plan.targetId);
  cJSON_SetNumberValue(processingRevision,
                       processingRevision->valueint + 1);
  batchMetadataSecondValue_ = printed(processing) + "\n";
  cJSON_Delete(processing);
  return !value.isEmpty() && !batchMetadataSecondValue_.isEmpty();
}

bool PokePodLinkService::startBatchMetadataCommit(
    const StoredCapsuleBatchPlan &plan, bool rollback) {
  const String key = batchTransactionId_ + "-" +
      String(static_cast<unsigned>(batchWork_.index)) +
      (rollback ? "-rollback" : "-apply");
  if (rollback) {
    CapsuleTransactionInput inputs[2];
    uint8_t count = 0;
    const String capsuleBackup = batchArtifactPath(
        batchWork_.index, ".capsule.bak");
    const String trashBackup = batchArtifactPath(
        batchWork_.index, ".trash.bak");
    if ((plan.flags & kBatchPlanCapsuleBackup) != 0 &&
        storageExists(capsuleBackup, StorageAccess::read)) {
      inputs[count++] = {String(plan.source) + "/capsule.json", nullptr,
                         capsuleBackup};
    }
    if ((plan.flags & kBatchPlanTrashBackup) != 0 &&
        storageExists(trashBackup, StorageAccess::read)) {
      inputs[count++] = {String(plan.source) + "/trash.json", nullptr,
                         trashBackup};
    }
    const String processingBackup = batchArtifactPath(
        batchWork_.index, ".processing.bak");
    if ((plan.flags & kBatchPlanProcessingBackup) != 0 &&
        storageExists(processingBackup, StorageAccess::read)) {
      inputs[count++] = {String(plan.source) + "/processing.json", nullptr,
                         processingBackup};
    }
    if (count == 0) return true;
    if (!transactionRunner_.startCommit(key.c_str(), inputs, count,
                                        storageOwner())) return false;
    batchPending_ = BatchPending::rollbackMetadata;
    return true;
  }
  String message;
  if (!buildBatchMetadata(plan, batchMetadataValue_, message)) {
    batchMessage_ = message;
    return false;
  }
  batchByteSource_.bind(batchMetadataValue_);
  CapsuleTransactionInput inputs[2];
  uint8_t count = 1;
  String firstTarget;
  if (strcmp(batchJournalState_.operation, "deleteCapsules") == 0) {
    firstTarget = String(plan.source) + "/trash.json";
  } else {
    const bool copied = strcmp(batchJournalState_.operation,
                               "copyCapsules") == 0;
    firstTarget = String(copied ? plan.target :
        ((strcmp(batchJournalState_.operation, "moveCapsules") == 0 ||
          strcmp(batchJournalState_.operation, "restoreCapsules") == 0 ||
          strcmp(batchJournalState_.operation, "deleteFolderToInbox") == 0)
             ? plan.target : plan.source)) + "/capsule.json";
  }
  inputs[0] = {firstTarget, &batchByteSource_, String()};
  if (strcmp(batchJournalState_.operation, "copyCapsules") == 0) {
    batchSecondByteSource_.bind(batchMetadataSecondValue_);
    inputs[1] = {String(plan.target) + "/processing.json",
                 &batchSecondByteSource_, String()};
    count = 2;
  }
  if (!transactionRunner_.startCommit(key.c_str(), inputs, count,
                                      storageOwner())) return false;
  batchPending_ = BatchPending::applyMetadata;
  return true;
}

bool PokePodLinkService::startBatchApply(size_t index) {
  if (!batchJournalStore_.readPlan(batchJournalState_,
                                   static_cast<uint16_t>(index), batchPlan_,
                                   storageOwner())) {
    batchMessage_ = "cannot read durable batch plan";
    return false;
  }
  if ((batchPlan_.flags & kBatchPlanNoOp) != 0) {
    finishBatchWork(true);
    return true;
  }
  const char *operation = batchJournalState_.operation;
  if (strcmp(operation, "beginMaintenance") == 0 ||
      strcmp(operation, "endMaintenance") == 0 ||
      strcmp(operation, "rescan") == 0) {
    finishBatchWork(true);
    return true;
  }
  if (strcmp(operation, "createFolder") == 0) {
    finishBatchWork(storageMkdir(batchPlan_.source));
    return true;
  }
  if (strcmp(operation, "renameFolder") == 0 ||
      strcmp(operation, "commitImport") == 0) {
    finishBatchWork(storageRename(batchPlan_.source, batchPlan_.target));
    return true;
  }
  if (strcmp(operation, "requeueTranscription") == 0) {
    finishBatchWork(library_->requeue(batchPlan_.id));
    return true;
  }
  if (strcmp(operation, "copyCapsules") == 0) {
    if (!batchTreeStepper_.beginCopy(
            *fs_, batchPlan_.source, batchPlan_.target, storageOwner(),
            [](void *context) {
              return static_cast<PokePodLinkService *>(context)
                  ->batchForegroundPermitted();
            }, this)) return false;
    batchPending_ = BatchPending::applyTree;
    batchPendingStep_ = 0;
    return true;
  }
  if (strcmp(operation, "purgeCapsules") == 0) {
    const String staging = batchPurgePath(batchPlan_);
    if (storageExists(staging, StorageAccess::read) ||
        !storageRename(batchPlan_.source, staging)) {
      batchMessage_ = "purge staging failed";
      return false;
    }
    finishBatchWork(true);
    return true;
  }
  if (strcmp(operation, "deleteCapsules") == 0) {
    batchPendingStep_ = 0;
    return startBatchMetadataCommit(batchPlan_, false);
  }
  if (strcmp(operation, "setFavorite") == 0 ||
      strcmp(operation, "addTags") == 0 ||
      strcmp(operation, "removeTags") == 0 ||
      strcmp(operation, "renameTag") == 0 ||
      strcmp(operation, "mergeTag") == 0 ||
      strcmp(operation, "deleteTag") == 0) {
    batchPendingStep_ = 2;
    return startBatchMetadataCommit(batchPlan_, false);
  }
  if (!storageRename(batchPlan_.source, batchPlan_.target)) {
    batchMessage_ = "capsule move failed";
    return false;
  }
  // Directory rename is one atomic primitive. Metadata publication starts on
  // a later poll so the current poll never combines two storage mutations.
  batchPending_ = BatchPending::applyPath;
  batchPendingStep_ = strcmp(operation, "restoreCapsules") == 0 ? 0 : 1;
  return true;
}

bool PokePodLinkService::startBatchRollback(size_t index) {
  if (!batchJournalStore_.readPlan(batchJournalState_,
                                   static_cast<uint16_t>(index), batchPlan_,
                                   storageOwner())) {
    batchMessage_ = "cannot read rollback plan";
    return false;
  }
  if ((batchPlan_.flags & kBatchPlanNoOp) != 0) {
    finishBatchWork(true);
    return true;
  }
  const char *operation = batchJournalState_.operation;
  if (strcmp(operation, "beginMaintenance") == 0 ||
      strcmp(operation, "endMaintenance") == 0 ||
      strcmp(operation, "rescan") == 0) {
    finishBatchWork(true);
    return true;
  }
  if (strcmp(operation, "createFolder") == 0) {
    const bool ok = !storageExists(batchPlan_.source, StorageAccess::read) ||
        storageRmdir(batchPlan_.source);
    finishBatchWork(ok);
    return true;
  }
  if (strcmp(operation, "renameFolder") == 0 ||
      strcmp(operation, "commitImport") == 0) {
    const bool sourceExists = storageExists(
        batchPlan_.source, StorageAccess::read);
    const bool targetExists = storageExists(
        batchPlan_.target, StorageAccess::read);
    const bool ok = (sourceExists && !targetExists) ||
        (!sourceExists && targetExists &&
         storageRename(batchPlan_.target, batchPlan_.source));
    finishBatchWork(ok);
    return true;
  }
  if (strcmp(operation, "requeueTranscription") == 0) {
    return startBatchMetadataCommit(batchPlan_, true);
  }
  if (strcmp(operation, "deleteFolderToInbox") == 0 &&
      !ensureDirectoryTree(parentPath(batchPlan_.source))) {
    batchMessage_ = "folder rollback path failed";
    return false;
  }
  if (strcmp(operation, "copyCapsules") == 0) {
    if (!storageExists(batchPlan_.target, StorageAccess::read)) {
      finishBatchWork(true);
      return true;
    }
    if (!batchTreeStepper_.beginRemove(*fs_, batchPlan_.target,
                                       storageOwner())) return false;
    batchPending_ = BatchPending::rollbackTree;
    return true;
  }
  if (strcmp(operation, "purgeCapsules") == 0) {
    const String staging = batchPurgePath(batchPlan_);
    const bool ok = !storageExists(staging, StorageAccess::read) ||
        storageRename(staging, batchPlan_.source);
    finishBatchWork(ok);
    return true;
  }
  if (strcmp(operation, "deleteCapsules") == 0) {
    if (storageExists(batchPlan_.target, StorageAccess::read) &&
        !storageExists(batchPlan_.source, StorageAccess::read) &&
        !storageRename(batchPlan_.target, batchPlan_.source)) return false;
    const bool ok = storageRemove(String(batchPlan_.source) + "/trash.json");
    finishBatchWork(ok);
    return true;
  }
  const bool metadata = strcmp(operation, "setFavorite") == 0 ||
      strcmp(operation, "addTags") == 0 ||
      strcmp(operation, "removeTags") == 0 ||
      strcmp(operation, "renameTag") == 0 ||
      strcmp(operation, "mergeTag") == 0 ||
      strcmp(operation, "deleteTag") == 0;
  if (!metadata && storageExists(batchPlan_.target, StorageAccess::read) &&
      !storageExists(batchPlan_.source, StorageAccess::read) &&
      !storageRename(batchPlan_.target, batchPlan_.source)) return false;
  return startBatchMetadataCommit(batchPlan_, true);
}

bool PokePodLinkService::startBatchFinalize() {
  if (strcmp(batchJournalState_.operation, "deleteFolderToInbox") != 0) {
    finishBatchWork(true);
    return true;
  }
  const char *folder = batchJournalState_.finalizeFolder;
  if (!safeFolder(folder, false)) {
    batchMessage_ = "invalid folder delete";
    return false;
  }
  const String source = String(kCapsuleRoot) + "/" + folder;
  if (!batchTreeStepper_.beginVerifyAbsent(
          *fs_, source, "*", storageOwner(),
          [](void *context) {
            return static_cast<PokePodLinkService *>(context)
                ->batchForegroundPermitted();
          }, this)) return false;
  batchPending_ = BatchPending::finalizeFolder;
  batchPendingStep_ = 0;
  return true;
}

bool PokePodLinkService::startBatchRollbackFinalize() {
  if (strcmp(batchJournalState_.operation, "deleteFolderToInbox") != 0 ||
      !safeFolder(batchJournalState_.finalizeFolder, false)) {
    finishBatchWork(false);
    return true;
  }
  const String source = String(kCapsuleRoot) + "/" +
      batchJournalState_.finalizeFolder;
  const String staging = batchFolderStagingPath();
  const bool sourceExists = storageExists(source, StorageAccess::read);
  const bool stagingExists = storageExists(staging, StorageAccess::read);
  const bool ok = (sourceExists && !stagingExists) ||
      (!sourceExists && stagingExists &&
       ensureDirectoryTree(parentPath(source)) &&
       storageRename(staging, source));
  finishBatchWork(ok);
  return true;
}

void PokePodLinkService::advanceBatchPending() {
  if (batchPending_ == BatchPending::persistResult) {
    const CapsuleTransactionPollResult result = transactionRunner_.poll(
        millis(), nullptr);
    if (result == CapsuleTransactionPollResult::progress ||
        result == CapsuleTransactionPollResult::wouldBlock) return;
    const bool persisted = result == CapsuleTransactionPollResult::committed;
    if (persisted) applyBatchResultSideEffects();
    finishBatchWork(persisted);
    return;
  }
  if (batchPending_ == BatchPending::backupCapsule ||
      batchPending_ == BatchPending::backupTrash ||
      batchPending_ == BatchPending::backupProcessing ||
      batchPending_ == BatchPending::applyTree ||
      batchPending_ == BatchPending::rollbackTree ||
      batchPending_ == BatchPending::finalizeFolder ||
      batchPending_ == BatchPending::cleanupTree) {
    const BatchPending pending = batchPending_;
    const LinkTreeStepper::Result result = batchTreeStepper_.poll();
    if (result == LinkTreeStepper::Result::progress ||
        result == LinkTreeStepper::Result::wouldBlock) return;
    if (result != LinkTreeStepper::Result::complete) {
      if (pending == BatchPending::finalizeFolder) {
        batchMessage_ = "folder contents changed during preflight";
      }
      finishBatchWork(false);
      return;
    }
    if (pending == BatchPending::finalizeFolder) {
      const String source = String(kCapsuleRoot) + "/" +
          batchJournalState_.finalizeFolder;
      const String staging = batchFolderStagingPath();
      if (batchPendingStep_ != 0 ||
          storageExists(staging, StorageAccess::read) ||
          !storageRename(source, staging)) {
        batchMessage_ = "folder staging failed";
        finishBatchWork(false);
      } else {
        batchPendingStep_ = 1;
        finishBatchWork(true);
      }
      return;
    }
    if (pending == BatchPending::backupCapsule &&
        (batchPlan_.flags & kBatchPlanTrashBackup) != 0) {
      if (!batchTreeStepper_.beginCopy(
              *fs_, String(batchPlan_.source) + "/trash.json",
              batchArtifactPath(batchWork_.index, ".trash.bak"),
              storageOwner(),
              [](void *context) {
                return static_cast<PokePodLinkService *>(context)
                    ->batchForegroundPermitted();
              }, this)) {
        finishBatchWork(false);
        return;
      }
      batchPending_ = BatchPending::backupTrash;
      return;
    }
    if ((pending == BatchPending::backupCapsule ||
         pending == BatchPending::backupTrash) &&
        (batchPlan_.flags & kBatchPlanProcessingBackup) != 0) {
      if (!batchTreeStepper_.beginCopy(
              *fs_, String(batchPlan_.source) + "/processing.json",
              batchArtifactPath(batchWork_.index, ".processing.bak"),
              storageOwner(),
              [](void *context) {
                return static_cast<PokePodLinkService *>(context)
                    ->batchForegroundPermitted();
              }, this)) {
        finishBatchWork(false);
        return;
      }
      batchPending_ = BatchPending::backupProcessing;
      return;
    }
    if (pending == BatchPending::applyTree) {
      batchPendingStep_ = 2;
      if (!startBatchMetadataCommit(batchPlan_, false)) {
        finishBatchWork(false);
      }
      return;
    }
    if (pending == BatchPending::cleanupTree) {
      if (strcmp(batchJournalState_.operation,
                 "deleteFolderToInbox") == 0 &&
          batchPendingStep_ == 4) {
        batchPending_ = BatchPending::cleanupArtifacts;
        return;
      }
      ++batchCleanupIndex_;
      batchPendingStep_ = 0;
      batchPending_ = BatchPending::cleanupArtifacts;
      return;
    }
    finishBatchWork(true);
    return;
  }

  if (batchPending_ == BatchPending::applyPath) {
    if (batchPendingStep_ == 0) {
      if (!storageRemove(String(batchPlan_.target) + "/trash.json")) {
        finishBatchWork(false);
        return;
      }
      batchPendingStep_ = 1;
      return;
    }
    batchPendingStep_ = 2;
    if (!startBatchMetadataCommit(batchPlan_, false)) {
      finishBatchWork(false);
    }
    return;
  }

  if (batchPending_ == BatchPending::applyMetadata ||
      batchPending_ == BatchPending::rollbackMetadata) {
    const bool rollback = batchPending_ == BatchPending::rollbackMetadata;
    const CapsuleTransactionPollResult result = transactionRunner_.poll(
        millis(), rollback ? nullptr : &transactionGate_);
    if (result == CapsuleTransactionPollResult::progress ||
        result == CapsuleTransactionPollResult::wouldBlock) return;
    if ((!rollback && result != CapsuleTransactionPollResult::committed) ||
        (rollback && result != CapsuleTransactionPollResult::committed)) {
      finishBatchWork(false);
      return;
    }
    if (!rollback &&
        strcmp(batchJournalState_.operation, "deleteCapsules") == 0 &&
        batchPendingStep_ == 0) {
      batchPendingStep_ = 1;
      if (!storageRename(batchPlan_.source, batchPlan_.target)) {
        finishBatchWork(false);
      } else {
        finishBatchWork(true);
      }
      return;
    }
    finishBatchWork(true);
    return;
  }

  if (batchPending_ == BatchPending::cleanupArtifacts) {
    if (batchCleanupIndex_ >= batchExecutor_.total()) {
      if (strcmp(batchJournalState_.operation,
                 "deleteFolderToInbox") == 0 &&
          batchExecutor_.success() && batchPendingStep_ != 4) {
        const String staging = batchFolderStagingPath();
        if (storageExists(staging, StorageAccess::read)) {
          if (!batchTreeStepper_.beginRemove(*fs_, staging,
                                              storageOwner())) {
            finishBatchWork(false);
            return;
          }
          batchPendingStep_ = 4;
          batchPending_ = BatchPending::cleanupTree;
          return;
        }
        batchPendingStep_ = 4;
        return;
      }
      if (strcmp(batchJournalState_.operation, "commitImport") == 0 &&
          batchExecutor_.success() && batchPendingStep_ != 5) {
        const String stagingParent = parentPath(batchPlan_.source);
        if (storageExists(stagingParent, StorageAccess::read) &&
            !storageRmdir(stagingParent)) {
          finishBatchWork(false);
          return;
        }
        batchPendingStep_ = 5;
        return;
      }
      const bool erased = batchExecutor_.preserveJournal() ||
          batchJournalStore_.erase(batchTransactionId_, storageOwner());
      finishBatchWork(erased);
      return;
    }
    if (!batchJournalStore_.readPlan(
            batchJournalState_, static_cast<uint16_t>(batchCleanupIndex_),
            batchPlan_, storageOwner())) {
      finishBatchWork(false);
      return;
    }
    if (batchPendingStep_ == 0) {
      if (!storageRemove(batchArtifactPath(
              batchCleanupIndex_, ".capsule.bak"))) {
        finishBatchWork(false);
        return;
      }
      batchPendingStep_ = 1;
      return;
    }
    if (batchPendingStep_ == 1) {
      if (!storageRemove(batchArtifactPath(
              batchCleanupIndex_, ".trash.bak"))) {
        finishBatchWork(false);
        return;
      }
      batchPendingStep_ = 2;
      return;
    }
    if (batchPendingStep_ == 2) {
      if (!storageRemove(batchArtifactPath(
              batchCleanupIndex_, ".processing.bak"))) {
        finishBatchWork(false);
        return;
      }
      batchPendingStep_ = 3;
      return;
    }
    if (strcmp(batchJournalState_.operation, "purgeCapsules") == 0 &&
        batchExecutor_.success()) {
      const String staging = batchPurgePath(batchPlan_);
      if (storageExists(staging, StorageAccess::read)) {
        if (!batchTreeStepper_.beginRemove(*fs_, staging, storageOwner())) {
          finishBatchWork(false);
          return;
        }
        batchPending_ = BatchPending::cleanupTree;
        return;
      }
    }
    ++batchCleanupIndex_;
    batchPendingStep_ = 0;
  }
}

bool PokePodLinkService::startBatchResultPersistence() {
  cJSON *result = cJSON_CreateObject();
  cJSON_AddNumberToObject(result, "schemaVersion", 1);
  cJSON_AddStringToObject(result, "commandId", batchTransactionId_.c_str());
  cJSON_AddStringToObject(result, "transactionId",
                         batchTransactionId_.c_str());
  cJSON_AddBoolToObject(result, "ok", batchExecutor_.success());
  cJSON_AddBoolToObject(result, "success", batchExecutor_.success());
  const bool queued = batchExecutor_.success() &&
      (strcmp(batchJournalState_.operation, "rescan") == 0 ||
       strcmp(batchJournalState_.operation, "requeueTranscription") == 0);
  cJSON_AddStringToObject(result, "message",
      batchExecutor_.success() ? (queued ? "queued" : "committed") :
      (batchMessage_.isEmpty() ? "batch command failed" :
                                 batchMessage_.c_str()));
  cJSON_AddStringToObject(result, "completedAt", board_->utcNow().c_str());
  batchResultValue_ = printed(result) + "\n";
  cJSON_Delete(result);
  const String path = String(kCapsuleSystem) + "/commands/results/" +
      batchTransactionId_ + ".json";
  batchByteSource_.bind(batchResultValue_);
  const CapsuleTransactionInput input{path, &batchByteSource_, String()};
  const String key = batchTransactionId_ + "-result";
  if (!transactionRunner_.startCommit(key.c_str(), &input, 1,
                                      storageOwner())) return false;
  batchPending_ = BatchPending::persistResult;
  return true;
}

void PokePodLinkService::applyBatchResultSideEffects() {
  if (!batchExecutor_.success()) return;
  if (batchPlan_.id[0] == '\0' && batchExecutor_.total() != 0) {
    if (!batchJournalStore_.readPlan(batchJournalState_, 0, batchPlan_,
                                     storageOwner())) return;
  }
  applyDurableCommandSideEffects(batchJournalState_.operation,
                                 batchPlan_.targetId);
}

void PokePodLinkService::applyDurableCommandSideEffects(
    const char *operation, const char *targetId) {
  if (operation == nullptr) return;
  if (strcmp(operation, "beginMaintenance") == 0 && isUuid(targetId)) {
    const bool changed = activeMaintenance_.isEmpty();
    if (changed || activeMaintenance_.equalsIgnoreCase(targetId)) {
      activeMaintenance_ = targetId;
      if (changed) maintenanceCompletion_.beginAccepted();
    }
  } else if (strcmp(operation, "endMaintenance") == 0 && isUuid(targetId)) {
    if (activeMaintenance_.isEmpty() ||
        activeMaintenance_.equalsIgnoreCase(targetId)) {
      activeMaintenance_ = "";
      maintenanceCompletion_.endResultPersisted(targetId);
    }
  } else if (strcmp(operation, "requeueTranscription") == 0) {
    tencent_->wake();
  } else if (strcmp(operation, "rescan") == 0) {
    (void)library_->requestScan();
  }
}

void PokePodLinkService::applyCompletedCommandSideEffects(void *jsonRoot) {
  cJSON *root = asJson(jsonRoot);
  if (root == nullptr || !cJSON_IsObject(root) ||
      jsonInt64(root, "schemaVersion") != 2 ||
      !sameUuid(jsonString(root, "transactionId"),
                commandLoadTransactionId_.c_str())) return;
  const char *operation = jsonString(root, "operation");
  const char *targetId = jsonString(root, "maintenanceId");
  applyDurableCommandSideEffects(operation, targetId);
}

bool PokePodLinkService::cleanupBatchArtifacts() {
  // Preserve an authority whose durable rollback checkpoint could not be
  // advanced. Boot recovery will retry it before accepting another mutation.
  if (batchExecutor_.preserveJournal()) return true;
  if (!storageRemove(batchCommandPath_)) return false;
  batchCleanupIndex_ = 0;
  batchPendingStep_ = 0;
  batchPending_ = BatchPending::cleanupArtifacts;
  return true;
}

void PokePodLinkService::finishBatchCommand() {
  const uint32_t requestId = batchRequestId_;
  const bool respond = batchExecutor_.responseAllowed() && sessionActive_ &&
      transferPermitted();
  if (batchExecutor_.preserveJournal() && batchRequestId_ == 0) {
    // Runtime IO/checkpoint failures do not prove corruption. Keep the
    // journal in place and fail closed so a later boot cannot replay an old
    // rollback over newer user data.
    startupRecoveryFailed_ = true;
  }
  if (batchExecutor_.preserveJournal() && batchRequestId_ != 0) {
    mutationRecoveryBlocked_ = true;
  }
  cJSON_Delete(asJson(batchJsonRoot_));
  batchJsonRoot_ = nullptr;
  batchRequestId_ = 0;
  batchCommandPath_ = "";
  batchTransactionId_ = "";
  batchMetadataValue_ = "";
  batchMetadataSecondValue_ = "";
  batchResultValue_ = "";
  batchMessage_ = "";
  batchPending_ = BatchPending::none;
  batchPlan_ = {};
  batchNextIdItem_ = nullptr;
  batchNextIdIndex_ = 0;
  batchJournalState_ = {};
  transactionGate_.reset();
  commandStorageActive_ = false;
  commandStorageReservation_.release();
  (void)library_->requestScan();
  if (respond) {
    sendOk(requestId, "\"accepted\":true");
    rememberCompleted(requestId);
    if (activeMaintenance_.isEmpty()) releaseRequestLease();
  }
}

void PokePodLinkService::abandonBatchCommand() {
  batchExecutor_.disconnect();
  transactionGate_.cancel();
}

bool PokePodLinkService::batchForegroundPermitted() const {
  if (batchExecutor_.phase() == LinkCommandExecutor::Phase::preflight ||
      batchExecutor_.phase() == LinkCommandExecutor::Phase::apply ||
      batchExecutor_.phase() == LinkCommandExecutor::Phase::finalize) {
    return sessionActive_ && transferPermitted();
  }
  return true;
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
  if (requestId != 0 && sessionActive_ && transferPermitted()) {
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

bool PokePodLinkService::cleanupPurgeStaging() {
  if (!startupPurgePending_) return true;
  if (fs_ == nullptr) return false;
  if (!startupPurgeReservation_) {
    startupPurgeReservation_ = StorageCoordinator::instance().reserve(
        storageOwner(), StorageAccess::mutation, 0);
    if (!startupPurgeReservation_) return false;
  }
  StorageIoLease lease = StorageCoordinator::instance().acquireIo(
      storageOwner(), StorageAccess::read, storageIoTimeout());
  if (!lease) return false;
  if (!startupPurgeDirectory_) {
    startupPurgeDirectory_ = fs_->open(kCapsuleStaging);
  }
  if (!startupPurgeDirectory_ || !startupPurgeDirectory_.isDirectory()) {
    if (startupPurgeDirectory_) startupPurgeDirectory_.close();
    startupPurgeReservation_.release();
    startupPurgePending_ = false;
    return true;
  }
  File entry = startupPurgeDirectory_.openNextFile();
  if (!entry) {
    startupPurgeDirectory_.close();
    startupPurgeReservation_.release();
    startupPurgePending_ = false;
    return true;
  }
  const String full = entry.name();
  const bool isDirectory = entry.isDirectory();
  entry.close();
  const int slash = full.lastIndexOf('/');
  const String name = slash >= 0 ? full.substring(slash + 1) : full;
  if (isDirectory && purgeStagingDirectoryName(name.c_str())) {
    queueDeferredTreeCleanup(String(kCapsuleStaging) + "/" + name);
  }
  return false;
}

void PokePodLinkService::queueDeferredTreeCleanup(const String &path) {
  if (path.isEmpty()) return;
  for (const String &queued : deferredTreeCleanupStack_) {
    if (queued == path) return;
  }
  if (deferredTreeCleanupStack_.empty()) deferredTreeCleanupFailures_ = 0;
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
    if (fs_->remove(path)) {
      deferredTreeCleanupFailures_ = 0;
      deferredTreeCleanupStack_.pop_back();
    } else if (++deferredTreeCleanupFailures_ >= 3) {
      deferredTreeCleanupBlocked_ = true;
      deferredTreeCleanupStack_.clear();
      if (log_ != nullptr) {
        log_->println("{\"event\":\"link_tree_cleanup_blocked\"}");
      }
    }
    return deferredTreeCleanupStack_.empty();
  }
  File entry = root.openNextFile();
  if (!entry) {
    root.close();
    if (fs_->rmdir(path)) {
      deferredTreeCleanupFailures_ = 0;
      deferredTreeCleanupStack_.pop_back();
    } else if (++deferredTreeCleanupFailures_ >= 3) {
      deferredTreeCleanupBlocked_ = true;
      deferredTreeCleanupStack_.clear();
      if (log_ != nullptr) {
        log_->println("{\"event\":\"link_tree_cleanup_blocked\"}");
      }
    }
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

bool PokePodLinkService::requestLinkRecordingStop(uint32_t requestId,
                                                   bool commit,
                                                   bool respond) {
  if (!linkOwnedRecording_ || captureRuntime_ == nullptr ||
      recorder_ == nullptr || captureRouter_ == nullptr) return false;
  if (linkRecordingStop_.active()) {
    if (!respond) linkRecordingStop_.suppressResponseAndAbort();
    return false;
  }
  if (!linkRecordingStop_.begin(requestId, commit, respond)) return false;
  // stop() may time out while the task is still completing its bounded I2S
  // read.  Ownership remains here; pollDeferredCleanup observes the eventual
  // stopped fact even if PokePodApp consumed the semaphore first.
  (void)captureRuntime_->stop(*log_);
  return true;
}

void PokePodLinkService::advanceLinkRecordingStart() {
  if (!linkRecordingStart_.active() || !linkOwnedRecording_ ||
      recorder_ == nullptr || captureRuntime_ == nullptr ||
      captureRouter_ == nullptr || audio_ == nullptr) return;
  CapsuleTransactionGate *gate =
      transport_ == LinkTransport::wifi || !sessionActive_ || quiesceRequested_
          ? &transactionGate_
          : nullptr;
  const RecorderStartPollResult result = recorder_->pollStart(
      *log_, millis(), gate);
  if (result == RecorderStartPollResult::pending) return;

  const uint32_t requestId = linkRecordingStart_.requestId();
  const uint32_t captureSessionId = linkRecordingStart_.captureSessionId();
  const String capsuleId = linkRecordingCapsuleId_;
  linkRecordingStart_.finish();
  linkRecordingCapsuleId_ = "";

  const bool transportAlive = sessionActive_ && !quiesceRequested_ &&
      transferPermitted();
  if (result == RecorderStartPollResult::started && transportAlive &&
      captureRuntime_->start(*audio_, captureSessionId, *log_)) {
    const String extra = "\"recording\":true,\"capsuleId\":\"" +
        capsuleId + "\"";
    sendOk(requestId, extra.c_str());
    rememberCompleted(requestId);
    if (activeMaintenance_.isEmpty()) releaseRequestLease();
    return;
  }

  if (linkRecordingStop_.active()) {
    linkRecordingStop_.suppressResponseAndAbort();
  } else {
    (void)requestLinkRecordingStop(requestId, false, transportAlive);
  }
}

void PokePodLinkService::advanceLinkRecordingStop() {
  if (!linkOwnedRecording_ ||
      captureRuntime_ == nullptr || recorder_ == nullptr ||
      captureRouter_ == nullptr) return;

  // USB has no time-window gate while its CDC session is alive. Once either
  // transport disconnects/quiesces, publish the latched cancellation gate so
  // an in-flight recorder transaction rolls back instead of committing after
  // its owner disappeared.
  CapsuleTransactionGate *recordingGate =
      transport_ == LinkTransport::wifi || !sessionActive_ || quiesceRequested_
          ? &transactionGate_
          : nullptr;
  // Link is the sole owner of Link recordings. Publishing the same gate on
  // every turn also lets the storage task advance periodic checkpoints while
  // capture is still active; the App only polls localApp-owned sessions.
  (void)recorder_->pollFinalize(*log_, millis(), recordingGate);
  if (linkRecordingStart_.active()) return;
  if (!linkRecordingStop_.active() && !recorder_->operationActive() &&
      recorder_->terminalResult().pending()) {
    (void)requestLinkRecordingStop(0, false, false);
  }
  if (!linkRecordingStop_.active()) return;

  if (linkRecordingStop_.awaitsCapture()) {
    if (captureRuntime_->running()) {
      if (captureRuntime_->finalizePending()) {
        (void)captureRuntime_->pollFinalize(*log_);
      }
      if (captureRuntime_->running()) return;
    }
    const bool drained = drainLinkCapture();
    const bool complete = linkRecordingStop_.commitRequested() && drained &&
        !captureRuntime_->incomplete();
    if (recorder_->recording()) {
      if (complete) {
        (void)recorder_->stop(*log_, recorder_->stopRequested()
            ? recorder_->requestedStopReason()
            : RecorderStopReason::user);
      } else {
        (void)recorder_->abortCapture(*log_);
      }
    }
    linkRecordingStop_.captureFinalized();
  }

  if (!linkRecordingStop_.awaitsRecorder()) return;
  (void)recorder_->pollFinalize(*log_, millis(), recordingGate);
  if (recorder_->operationActive()) return;

  RecorderOutcome outcome;
  if (!recorder_->takeTerminalResult(outcome)) return;

  const uint32_t requestId = linkRecordingStop_.requestId();
  const bool respond = linkRecordingStop_.shouldRespond() && sessionActive_ &&
      transferPermitted();
  const bool committed = outcome.success();
  captureRouter_->release(AudioCaptureOwner::localCapsule);
  linkOwnedRecording_ = false;
  linkRecordingStop_.finish();
  transactionGate_.reset();

  if (!respond || requestId == 0) return;
  if (committed && library_->requestScan()) {
    sendOk(requestId,
           "\"recording\":false,\"queued\":true,"
           "\"indexRefresh\":\"queued\"");
  } else if (committed) {
    sendError(requestId, "recording committed but index refresh failed");
  } else {
    sendError(requestId, "recording commit failed");
  }
  rememberCompleted(requestId);
  if (activeMaintenance_.isEmpty()) releaseRequestLease();
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
  const LinkCommandExecutor::Phase phase = batchExecutor_.phase();
  const bool foreground = commandLoadState_ != CommandLoadState::none ||
      phase == LinkCommandExecutor::Phase::preflight ||
      phase == LinkCommandExecutor::Phase::apply ||
      phase == LinkCommandExecutor::Phase::finalize;
  if (foreground && !transferPermitted()) return false;
  StorageIoLease lease = StorageCoordinator::instance().acquireIo(
      storageOwner(), access, storageIoTimeout());
  if (!lease) return false;
  const bool exists = fs_->exists(path);
  lease.release();
  return (!foreground || transferPermitted()) && exists;
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
