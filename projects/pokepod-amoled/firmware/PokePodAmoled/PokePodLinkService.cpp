#include "PokePodLinkService.h"

#include <SD_MMC.h>
#include <cJSON.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <mbedtls/sha256.h>
#include <algorithm>
#include <vector>

#include "AudioPipeline.h"
#include "AudioCaptureRuntime.h"
#include "AudioCaptureDispatcher.h"
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
constexpr size_t kReadPageFiles = 12;
constexpr size_t kLinkTxFrameBytes = kLinkHeaderBytes + kLinkMaxDataBytes;
constexpr size_t kLinkPendingControlBytes =
    kLinkHeaderBytes + kLinkMaxControlBytes;
constexpr size_t kLinkWriteSliceBytes = 512;
constexpr const char *kTerminalQueueError =
    "{\"status\":\"error\",\"version\":2,"
    "\"message\":\"response unavailable\"}";
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
  return cJSON_IsNumber(item) ? static_cast<int64_t>(item->valuedouble) : fallback;
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
                               AudioCaptureDispatcher *captureDispatcher,
                               const CapabilityRegistry *capabilities) {
  stream_ = &stream;
  fs_ = &fs;
  board_ = &board;
  audio_ = &audio;
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
  provisioningCoordinator_ = provisioningCoordinator;
  log_ = &log;
  recordingSession_.begin(audio, captureRuntime, captureDispatcher,
                          captureRouter, bleVoice, recorder, library, log);
  diagnostics_.bind(board, audio, usb, bleVoice, dashboard, library, recorder,
                    config, wifi, tencent, provisioningDiagnostics,
                    powerDiagnostics, power, provisioningCoordinator,
                    capabilities);
  fileTransfer_.bind(*this);
  coordinator_ = coordinator;
  transport_ = transport;
  pairingProvider_ = pairingProvider;
  transferGate_ = transferGate;
  writeChannel_ = writeChannel;
  connectionGeneration_ = 0;
  nextConnectionGeneration_ = 0;
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

uint32_t PokePodLinkService::activateConnectionGeneration() {
  if (connectionGeneration_ != 0) return connectionGeneration_;
  ++nextConnectionGeneration_;
  if (nextConnectionGeneration_ == 0) ++nextConnectionGeneration_;
  connectionGeneration_ = nextConnectionGeneration_;
  // Request ids are scoped to one physical CDC/TLS connection.  Clear the
  // replay cache only when a fresh transport epoch is observed, after any
  // previous operation has finished owner-scoped cleanup.
  completed_.clear();
  return connectionGeneration_;
}

LinkOperationAdmission PokePodLinkService::admitLinkOperation(
    uint32_t requestId) {
  const LinkTransport lifecycleTransport =
      transport_ == LinkTransport::none ? LinkTransport::usb : transport_;
  const uint32_t generation = activateConnectionGeneration();
  uint32_t absoluteDeadlineMs = 0;
  const bool deadlineArmed = transferGate_ != nullptr &&
      transferGate_->absoluteDeadline(absoluteDeadlineMs);

  // An active or retained lifecycle decides same-request/busy before touching
  // the external coordinator. This prevents a rejected request from releasing
  // the operation that is already in flight.
  if (operation_.active() ||
      operation_.ownsResource(LinkOperationResource::coordinator)) {
    return operation_.admit(requestId, lifecycleTransport, generation, false,
                            deadlineArmed, absoluteDeadlineMs);
  }

  bool coordinatorOwned = false;
  if (coordinator_ != nullptr && transport_ != LinkTransport::none) {
    if (!coordinator_->acquire(transport_)) {
      return LinkOperationAdmission::busy;
    }
    coordinatorOwned = true;
  }
  const LinkOperationAdmission admission = operation_.admit(
      requestId, lifecycleTransport, generation, coordinatorOwned,
      deadlineArmed, absoluteDeadlineMs);
  if (admission != LinkOperationAdmission::accepted && coordinatorOwned) {
    coordinator_->release(transport_);
  }
  return admission;
}

bool PokePodLinkService::operationOwns(uint32_t requestId) const {
  const LinkTransport lifecycleTransport =
      transport_ == LinkTransport::none ? LinkTransport::usb : transport_;
  return connectionGeneration_ != 0 &&
      operation_.owns(requestId, lifecycleTransport, connectionGeneration_);
}

void PokePodLinkService::cancelLinkOperation(
    LinkOperationCancelReason reason) {
  if (operation_.active()) {
    operation_.cancel(reason);
    if (connectionGeneration_ != 0) {
      operation_.discardQueuedFrames(connectionGeneration_);
    }
    return;
  }
  if (connectionGeneration_ == 0) return;
  const LinkTransport lifecycleTransport =
      transport_ == LinkTransport::none ? LinkTransport::usb : transport_;
  (void)operation_.cancelRetainedCoordinator(
      lifecycleTransport, connectionGeneration_, reason);
}

void PokePodLinkService::advanceLinkOperationSettlement() {
  const LinkOperationSettlement proposal = operation_.settlement();
  if (!proposal.ready || !operation_.beginRelease()) return;
  LinkOperationSettlement pending = operation_.settlement();
  if (pending.rememberCompleted) {
    const uint32_t requestId = operation_.requestId();
    // A disconnected epoch has no live replay history.  A late cleanup from
    // that epoch is acknowledged locally without contaminating a later
    // connection's request-id cache.
    const bool currentEpoch = sessionActive_ && connectionGeneration_ != 0 &&
        operation_.connectionGeneration() == connectionGeneration_;
    const bool completed = !currentEpoch || requestId == 0 ||
        completed_.contains(requestId) || completed_.complete(requestId);
    (void)operation_.acknowledgeCompletion(completed);
  }
  pending = operation_.settlement();
  if (pending.releaseRequest) {
    (void)operation_.acknowledgeRelease(LinkOperationResource::request, true);
  }
  pending = operation_.settlement();
  if (pending.releaseCoordinator) {
    bool released = coordinator_ == nullptr || transport_ == LinkTransport::none;
    if (coordinator_ != nullptr && transport_ != LinkTransport::none) {
      if (coordinator_->owner() == transport_) {
        coordinator_->release(transport_);
      }
      released = coordinator_->owner() == LinkTransport::none;
    }
    (void)operation_.acknowledgeRelease(
        LinkOperationResource::coordinator, released);
  }
  const bool released = operation_.finishRelease();
  if (released && !sessionActive_) connectionGeneration_ = 0;
}

void PokePodLinkService::disconnect() {
  cancelLinkOperation(quiesceRequested_
      ? LinkOperationCancelReason::quiesce
      : LinkOperationCancelReason::disconnect);
  if (batchExecutor_.active()) abandonBatchCommand();
  manifestResponseRequestId_ = 0;
  manifestResponseJson_ = "";
  manifestFailureRequestId_ = 0;
  manifestFailureMessage_ = "";
  recordingSession_.disconnect(operation_, transactionGate_);
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
  fileTransfer_.abort();
  sessionActive_ = false;
  incomingCleanupRespond_ = false;
  if (incomingKind_ != IncomingKind::none || incomingCleanupPending_) {
    failIncoming("transport disconnected");
  }
  activeMaintenance_ = "";
  maintenanceCompletion_.disconnect();
  commandLoadRespond_ = false;
  resetFrame();
  // The core keeps the old generation until its resources settle.  The
  // service generation represents only the live physical connection.
  connectionGeneration_ = 0;
  advanceLinkOperationSettlement();
  if (!operation_.active() &&
      !operation_.ownsResource(LinkOperationResource::coordinator)) {
    connectionGeneration_ = 0;
  }
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
      fileTransfer_.quiesced() &&
      !manifestStepper_.active() && !manifestCleanupPending_ &&
      !transactionRunner_.active() &&
      transactionPurpose_ == TransactionPurpose::none &&
      !batchExecutor_.active() && batchPending_ == BatchPending::none &&
      deferredCommandFiles_.empty() && deferredTreeCleanupStack_.empty() &&
      !commandCleanupPending_ && !commandStorageActive_ &&
      recordingSession_.quiesced() &&
      !operation_.active() &&
      !operation_.ownsResource(LinkOperationResource::coordinator);
}

void PokePodLinkService::pollDeferredCleanup() {
  advanceCommandLoad();
  advanceBatchCommand();
  advanceTransactionRunner();
  advanceBatchStartupRecovery();
  advanceStartupPartCleanup();
  cleanupPurgeStaging();
  handleLinkRecordingEvent(recordingSession_.poll(
      operation_, transactionGate_, transport_, transferGate_, sessionActive_,
      quiesceRequested_));
  if (incomingCleanupPending_) cleanupIncomingStorage();
  (void)fileTransfer_.pollCleanup();
  if (manifestCleanupPending_) cleanupManifestStorage();
  stepDeferredFileCleanup();
  if (!startupPurgePending_ && deferredCommandFiles_.empty()) {
    stepDeferredTreeCleanup();
  }
  finishCommandStorageCleanup();
  advanceLinkOperationSettlement();
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
  if (incomingCleanupPending_ || fileTransfer_.cleanupPending() ||
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
  recordingSession_.observeAutomaticStop(operation_, transactionGate_);
  if (!transferPermitted()) {
    disconnect();
    return;
  }
  if (txStepper_.active()) {
    advanceTransmit(nowMs);
    return;
  }
  if (fileTransfer_.dataReady()) {
    fileTransfer_.advance();
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
        activateConnectionGeneration();
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
  operation_.advance(LinkOperationState::receiving);
  operation_.ownResource(LinkOperationResource::storageReservation);
  operation_.ownResource(LinkOperationResource::file);
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
  operation_.releaseResource(LinkOperationResource::file);
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
  operation_.advance(LinkOperationState::durableCommit);
  operation_.ownResource(LinkOperationResource::transaction);
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
  operation_.releaseResource(LinkOperationResource::transaction);
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
  operation_.releaseResource(LinkOperationResource::storageReservation);
  if (kind == IncomingKind::command) {
    handleCommandFile(requestId, finalPath, transaction);
    if (commandCleanupPending_) return;
  } else if (respond && kind == IncomingKind::systemFont) {
    sendOk(requestId, "\"installed\":true,\"rebootRequired\":true");
  } else if (respond) {
    sendOk(requestId);
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
  operation_.releaseResource(LinkOperationResource::file);
  operation_.releaseResource(LinkOperationResource::storageReservation);
  operation_.releaseResource(LinkOperationResource::transaction);
  if (respond && requestId != 0) {
    sendError(requestId, message.c_str());
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
      String(retryAfterMs) + "}", false);
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
  return sendJson(requestId, json, true);
}

bool PokePodLinkService::sendJson(uint32_t requestId, const String &json,
                                  bool completionEligible) {
  return sendJson(requestId, json, completionEligible, false);
}

bool PokePodLinkService::sendJson(uint32_t requestId, const String &json,
                                  bool completionEligible,
                                  bool preserveForFallback) {
  if (operationOwns(requestId)) {
    if (operation_.state() == LinkOperationState::receiving) {
      operation_.advance(LinkOperationState::processing);
    }
    operation_.retainCoordinatorAfterTerminal(!activeMaintenance_.isEmpty());
  }
  return sendFrame(LinkFrameType::responseJson, 0, requestId,
                   reinterpret_cast<const uint8_t *>(json.c_str()),
                   json.length(), LinkOperationFrameRole::terminal,
                   completionEligible, preserveForFallback);
}

bool PokePodLinkService::sendTerminalOrDisconnect(
    uint32_t requestId, const String &json, bool completionEligible) {
  if (sendJson(requestId, json, completionEligible, true)) return true;
  if (operationOwns(requestId) && operation_.responseAllowed() &&
      sendJson(requestId, String(kTerminalQueueError), false)) {
    return false;
  }
  if (operationOwns(requestId)) {
    operation_.cancel(LinkOperationCancelReason::operationFailure);
    operation_.discardQueuedFrames(connectionGeneration_);
  }
  disconnect();
  return false;
}

bool PokePodLinkService::sendEvent(uint32_t requestId, const String &json) {
  return sendFrame(LinkFrameType::eventJson, 0, requestId,
                   reinterpret_cast<const uint8_t *>(json.c_str()),
                   json.length(), LinkOperationFrameRole::progress);
}

bool PokePodLinkService::sendFrame(LinkFrameType type, uint16_t flags,
                                   uint32_t requestId,
                                   const uint8_t *payload, size_t size,
                                   LinkOperationFrameRole role,
                                   bool completionEligible,
                                   bool preserveForFallback) {
  return queueFrame(type, flags, requestId, payload, size,
                    LinkFileTransferFrameCompletion::none, role,
                    completionEligible,
                    preserveForFallback);
}

bool PokePodLinkService::queueFrame(LinkFrameType type, uint16_t flags,
                                    uint32_t requestId,
                                    const uint8_t *payload, size_t size,
                                    LinkFileTransferFrameCompletion completion,
                                    LinkOperationFrameRole role,
                                    bool completionEligible,
                                    bool preserveForFallback) {
  const bool lifecycleOwned = operationOwns(requestId);
  const auto failOwnedQueue = [&]() {
    if (!lifecycleOwned) return;
    operation_.cancel(LinkOperationCancelReason::operationFailure);
    operation_.discardQueuedFrames(connectionGeneration_);
  };
  const auto failQueue = [&]() {
    if (!preserveForFallback) failOwnedQueue();
  };
  if (stream_ == nullptr || !transferPermitted() || size >
      (type == LinkFrameType::data ? kLinkMaxDataBytes : kLinkMaxControlBytes)) {
    failQueue();
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
  LinkFileTransferFrameCompletion *targetCompletion = nullptr;
  LinkOperationFrameRole *targetRole = nullptr;
  uint32_t *targetGeneration = nullptr;
  if (!txStepper_.active() && txFrameBytes_ == 0) {
    target = txFrame_;
    targetBytes = &txFrameBytes_;
    targetCompletion = &txCompletion_;
    targetRole = &txFrameRole_;
    targetGeneration = &txFrameGeneration_;
  } else if (type != LinkFrameType::data && pendingControlBytes_ == 0) {
    target = pendingControlFrame_;
    targetBytes = &pendingControlBytes_;
    targetCompletion = &pendingControlCompletion_;
    targetRole = &pendingControlRole_;
    targetGeneration = &pendingControlGeneration_;
  } else {
    failQueue();
    return false;
  }
  if (target == nullptr ||
      !encodeLinkHeader(header, target, kLinkHeaderBytes)) {
    failQueue();
    return false;
  }
  if (size > 0) memcpy(target + kLinkHeaderBytes, payload, size);
  *targetBytes = kLinkHeaderBytes + size;
  *targetCompletion = completion;
  *targetRole = role;
  *targetGeneration = lifecycleOwned ? connectionGeneration_ : 0;
  if (target == txFrame_ && !txStepper_.beginFrame(*targetBytes)) {
    *targetBytes = 0;
    *targetCompletion = LinkFileTransferFrameCompletion::none;
    *targetRole = LinkOperationFrameRole::progress;
    *targetGeneration = 0;
    failQueue();
    return false;
  }
  if (lifecycleOwned && !operation_.queueFrame(
          role, connectionGeneration_, millis(), completionEligible)) {
    if (target == txFrame_) txStepper_.cancel();
    *targetBytes = 0;
    *targetCompletion = LinkFileTransferFrameCompletion::none;
    *targetRole = LinkOperationFrameRole::progress;
    *targetGeneration = 0;
    failQueue();
    return false;
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

  const LinkFileTransferFrameCompletion completion = txCompletion_;
  const LinkOperationFrameRole role = txFrameRole_;
  const uint32_t generation = txFrameGeneration_;
  txFrameBytes_ = 0;
  txCompletion_ = LinkFileTransferFrameCompletion::none;
  txFrameRole_ = LinkOperationFrameRole::progress;
  txFrameGeneration_ = 0;
  if (generation != 0) {
    (void)operation_.frameDrained(role, generation, millis());
  }
  fileTransfer_.onFrameSent(completion);

  if (!txStepper_.active() && pendingControlBytes_ > 0) {
    memcpy(txFrame_, pendingControlFrame_, pendingControlBytes_);
    txFrameBytes_ = pendingControlBytes_;
    txCompletion_ = pendingControlCompletion_;
    txFrameRole_ = pendingControlRole_;
    txFrameGeneration_ = pendingControlGeneration_;
    pendingControlBytes_ = 0;
    pendingControlCompletion_ = LinkFileTransferFrameCompletion::none;
    pendingControlRole_ = LinkOperationFrameRole::progress;
    pendingControlGeneration_ = 0;
    if (!txStepper_.beginFrame(txFrameBytes_)) {
      if (txFrameGeneration_ != 0) {
        operation_.cancel(LinkOperationCancelReason::operationFailure);
        operation_.discardQueuedFrames(txFrameGeneration_);
      }
      txFrameBytes_ = 0;
      txCompletion_ = LinkFileTransferFrameCompletion::none;
      txFrameRole_ = LinkOperationFrameRole::progress;
      txFrameGeneration_ = 0;
    }
  }
  advanceLinkOperationSettlement();
}

bool PokePodLinkService::linkFileTransferPermitted() const {
  return transferPermitted();
}

bool PokePodLinkService::linkFileTransmitIdle() const {
  return !txStepper_.active() && txFrameBytes_ == 0 &&
      pendingControlBytes_ == 0;
}

void PokePodLinkService::linkFileCancelForDeadline(uint32_t requestId) {
  if (!operationOwns(requestId)) return;
  operation_.cancel(LinkOperationCancelReason::deadline);
  operation_.discardQueuedFrames(connectionGeneration_);
}

void PokePodLinkService::linkFileSendBusy(uint32_t requestId) {
  sendBusy(requestId);
}

void PokePodLinkService::linkFileSendError(uint32_t requestId,
                                           const char *message) {
  sendError(requestId, message);
}

bool PokePodLinkService::linkFileQueueFrame(
    LinkFrameType type, uint16_t flags, uint32_t requestId,
    const uint8_t *payload, size_t size,
    LinkFileTransferFrameCompletion completion,
    LinkOperationFrameRole role, bool completionEligible) {
  return queueFrame(type, flags, requestId, payload, size, completion, role,
                    completionEligible);
}

void PokePodLinkService::linkFileDisconnectTransport() { disconnect(); }

void PokePodLinkService::linkFileClaimResources(uint32_t requestId) {
  if (!operationOwns(requestId)) return;
  (void)operation_.advance(LinkOperationState::processing);
  operation_.ownResource(LinkOperationResource::storageReservation);
  operation_.ownResource(LinkOperationResource::file);
}

void PokePodLinkService::linkFileReleaseResources() {
  operation_.releaseResource(LinkOperationResource::file);
  operation_.releaseResource(LinkOperationResource::storageReservation);
}

void PokePodLinkService::linkFileAdvanceSettlement() {
  advanceLinkOperationSettlement();
}

void PokePodLinkService::linkFileCancelTransmitFrames() {
  txStepper_.cancel();
  txFrameBytes_ = 0;
  txCompletion_ = LinkFileTransferFrameCompletion::none;
  txFrameRole_ = LinkOperationFrameRole::progress;
  txFrameGeneration_ = 0;
  pendingControlBytes_ = 0;
  pendingControlCompletion_ = LinkFileTransferFrameCompletion::none;
  pendingControlRole_ = LinkOperationFrameRole::progress;
  pendingControlGeneration_ = 0;
}

fs::FS *PokePodLinkService::linkFileSystem() { return fs_; }

StorageOwner PokePodLinkService::linkFileStorageOwner() const {
  return storageOwner();
}

uint32_t PokePodLinkService::linkFileStorageIoTimeout() const {
  return storageIoTimeout();
}

uint8_t *PokePodLinkService::linkFilePayloadBuffer() { return payload_; }

size_t PokePodLinkService::linkFilePayloadCapacity() const {
  return kLinkMaxDataBytes;
}

void PokePodLinkService::linkFileResultFetched(const char *transactionId,
                                               bool fullySent) {
  (void)maintenanceCompletion_.resultFetched(transactionId, fullySent);
}

void PokePodLinkService::handleLinkRecordingEvent(
    const LinkRecordingEvent &event) {
  if (!event.respond || event.requestId == 0) return;
  switch (event.kind) {
    case LinkRecordingEventKind::startReady: {
      const String extra = "\"recording\":true,\"capsuleId\":\"" +
          event.capsuleId + "\"";
      sendOk(event.requestId, extra.c_str());
      break;
    }
    case LinkRecordingEventKind::stopCommitted:
      sendOk(event.requestId,
             "\"recording\":false,\"queued\":true,"
             "\"indexRefresh\":\"queued\"");
      break;
    case LinkRecordingEventKind::stopCommittedIndexFailed:
      sendError(event.requestId,
                "recording committed but index refresh failed");
      break;
    case LinkRecordingEventKind::stopFailed:
      sendError(event.requestId, "recording commit failed");
      break;
    case LinkRecordingEventKind::none:
      break;
  }
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

bool PokePodLinkService::foregroundBusy() const {
  if (recorder_ == nullptr || tencent_ == nullptr || bleVoice_ == nullptr ||
      captureRouter_ == nullptr) return true;
  if (audio_ != nullptr && audio_->playing()) return true;
  return linkForegroundBusy(recorder_->recording(), tencent_->working(),
                            bleVoice_->streaming(),
                            captureRouter_->available());
}

}  // namespace pokepod
