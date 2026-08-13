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

String parentPath(const String &path) {
  const int slash = path.lastIndexOf('/');
  return slash <= 0 ? String("/") : path.substring(0, slash);
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
