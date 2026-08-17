#include "PokePodLinkService.h"

#include <cJSON.h>
#include <algorithm>
#include <esp_timer.h>

#include "RuntimeDiagnostics.h"
#include "TencentWorker.h"
#include "UsbLinkBridge.h"
#include "UsbLinkSessionReconcile.h"

namespace pokepod {
namespace {

constexpr size_t kLinkWriteSliceBytes = 512;
constexpr size_t kLinkReadSliceBytes = 128;
constexpr size_t kLinkPollBudgetBytes = 32768;
constexpr uint64_t kLinkPollBudgetUs = 2000;
constexpr const char *kTerminalQueueError =
    "{\"status\":\"error\",\"version\":2,"
    "\"message\":\"response unavailable\"}";

String printed(cJSON *root) {
  char *value = cJSON_PrintUnformatted(root);
  const String result = value == nullptr ? String() : String(value);
  cJSON_free(value);
  return result;
}

uint64_t linkPollNowUs(void *) {
  return static_cast<uint64_t>(esp_timer_get_time());
}

}  // namespace

uint32_t PokePodLinkService::activateConnectionGeneration() {
  if (connectionGeneration_ != 0) return connectionGeneration_;
  ++nextConnectionGeneration_;
  if (nextConnectionGeneration_ == 0) ++nextConnectionGeneration_;
  connectionGeneration_ = nextConnectionGeneration_;
  // Request ids are scoped to one physical CDC/TLS connection.  Clear the
  // replay cache only when a fresh transport epoch is observed, after any
  // previous operation has finished owner-scoped cleanup.
  completed_.clear();
  liveness_.openSession(connectionGeneration_, millis());
  return connectionGeneration_;
}

String PokePodLinkService::linkProbeJson() const {
  const LinkLivenessSnapshot &probe = liveness_.snapshot();
  String value = "\"linkSessionActive\":" +
      String(sessionActive_ ? "true" : "false") +
      ",\"linkGeneration\":" + String(connectionGeneration_) +
      ",\"linkUsbHostGeneration\":" + String(usbHostSessionGeneration_) +
      ",\"linkRequestId\":" + String(operation_.requestId()) +
      ",\"linkOperationState\":" +
      String(static_cast<unsigned>(operation_.state())) +
      ",\"linkQueuedFrames\":" + String(operation_.queuedFrameCount()) +
      ",\"linkCapturePreparePending\":" +
      String(recordingSession_.capturePreparePending() ? "true" : "false") +
      ",\"linkCapturePrepareAttempted\":" +
      String(recordingSession_.capturePrepareAttempted() ? "true" : "false") +
      ",\"linkCapturePrepared\":" +
      String(recordingSession_.capturePrepared() ? "true" : "false") +
      ",\"linkCaptureStopIssued\":" +
      String(recordingSession_.captureStopIssued() ? "true" : "false") +
      ",\"linkLastProgressMs\":" + String(probe.lastProgressMs) +
      ",\"linkRecoveryCount\":" + String(probe.recoveryCount) +
      ",\"linkLastRecoveryMs\":" + String(probe.lastRecoveryMs) +
      ",\"linkLastStall\":" +
      String(static_cast<unsigned>(probe.lastStall));
  return value;
}

bool PokePodLinkService::recoverStalledLink(uint32_t nowMs) {
  const bool receivePartial = receivePhase_ != ReceivePhase::magic ||
      deferredRxByte_ >= 0;
  const bool transmitPending = txStepper_.active() || txFrameBytes_ != 0 ||
      pendingControlBytes_ != 0;
  const bool operationRecoverable = operation_.operationalResourcesDrained() &&
      recordingSession_.quiesced() && !transactionRunner_.active() &&
      !fileTransfer_.cleanupPending() && !manifestCleanupPending_ &&
      !incomingCleanupPending_;
  const LinkLivenessStall stall = liveness_.observe(
      nowMs, sessionActive_, operation_.requestId(),
      operation_.queuedFrameCount(), receivePartial, transmitPending,
      operationRecoverable);
  if (stall == LinkLivenessStall::none) return false;

  const uint32_t detail1 =
      (static_cast<uint32_t>(stall) << 24U) |
      ((operation_.queuedFrameCount() & 0xffffU) << 8U) |
      static_cast<uint32_t>(operation_.state());
  if (runtimeDiagnostics_ != nullptr && log_ != nullptr) {
    (void)runtimeDiagnostics_->record(
        RuntimeDiagnosticSubsystem::link,
        RuntimeDiagnosticStage::linkStallRecovery,
        RuntimeDiagnosticOutcome::failure, operation_.requestId(), detail1,
        *log_);
  }
  liveness_.recovered(stall, nowMs);
  if (transport_ == LinkTransport::usb && usb_ != nullptr) {
    usb_->discardHostSessionBuffers();
  }
  disconnect();
  return true;
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
  // An accepted reboot is a device-lifecycle intent.  Transport teardown may
  // cancel frames and Link operations, but it must not cancel the reboot.
  cancelLinkOperation(quiesceRequested_
      ? LinkOperationCancelReason::quiesce
      : LinkOperationCancelReason::disconnect);
  if (firmwareUpdate_.active() || firmwareUpdateRequestId_ != 0) {
    firmwareUpdate_.abort();
    firmwareUpdateRequestId_ = 0;
    operation_.releaseResource(LinkOperationResource::firmwareUpdate);
  }
  if (batchExecutor_.active()) abandonBatchCommand();
  resetMetadataRead();
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
  usbHostSessionGeneration_ = 0;
  incomingCleanupRespond_ = false;
  if (incomingKind_ != IncomingKind::none || incomingCleanupPending_) {
    failIncoming("transport disconnected");
  }
  activeMaintenance_ = "";
  maintenanceCompletion_.disconnect();
  commandLoadRespond_ = false;
  deferredRxByte_ = -1;
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
  return quiesceRequested_ && !sessionActive_ && cleanupDrained();
}

bool PokePodLinkService::cleanupDrained() const {
  return commandLoadState_ == CommandLoadState::none &&
      !firmwareUpdate_.active() && firmwareUpdateRequestId_ == 0 &&
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

bool PokePodLinkService::deviceLifecycleRestartReady() const {
  return !txStepper_.active() && txFrameBytes_ == 0 &&
      pendingControlBytes_ == 0 && operation_.queuedFrameCount() == 0 &&
      cleanupDrained() && !maintenanceActive();
}

bool PokePodLinkService::pollDeferredCleanup(LinkPollPhaseGate &gate) {
  if (!gate.run([&]() { advanceCommandLoad(); })) return false;
  if (!gate.run([&]() { advanceBatchCommand(); })) return false;
  if (!gate.run([&]() { advanceTransactionRunner(); })) return false;
  if (!gate.run([&]() { advanceBatchStartupRecovery(); })) return false;
  if (!gate.run([&]() { advanceStartupPartCleanup(); })) return false;
  if (!gate.run([&]() { (void)cleanupPurgeStaging(); })) return false;
  if (!gate.run([&]() {
        handleLinkRecordingEvent(recordingSession_.poll(
            operation_, transactionGate_, transport_, transferGate_,
            sessionActive_, quiesceRequested_));
      })) return false;
  if (incomingCleanupPending_ &&
      !gate.run([&]() { (void)cleanupIncomingStorage(); })) return false;
  if (!gate.run([&]() { (void)fileTransfer_.pollCleanup(); })) return false;
  if (manifestCleanupPending_ &&
      !gate.run([&]() { (void)cleanupManifestStorage(); })) return false;
  if (!gate.run([&]() { (void)stepDeferredFileCleanup(); })) return false;
  if (!startupPurgePending_ && deferredCommandFiles_.empty()) {
    if (!gate.run([&]() { (void)stepDeferredTreeCleanup(); })) return false;
  }
  if (!gate.run([&]() { finishCommandStorageCleanup(); })) return false;
  if (!gate.run([&]() { advanceLinkOperationSettlement(); })) return false;
  if (!startupReady_ && !startupRecoveryFailed_ &&
      !startupBatchRecoveryPending_ && !startupPartCleanupPending_ &&
      !startupPurgePending_ && deferredTreeCleanupStack_.empty()) {
    startupReady_ = true;
  }
  return gate.checkpoint();
}

void PokePodLinkService::pollDeferredCleanup() {
  const uint64_t startedUs = linkPollNowUs(nullptr);
  LinkPollBudget budget(kLinkPollBudgetBytes, kLinkPollBudgetUs, startedUs);
  LinkPollPhaseGate gate(budget, linkPollNowUs);
  (void)pollDeferredCleanup(gate);
}

void PokePodLinkService::poll(uint32_t nowMs) {
  const uint64_t startedUs = linkPollNowUs(nullptr);
  LinkPollBudget budget(kLinkPollBudgetBytes, kLinkPollBudgetUs, startedUs);
  LinkPollPhaseGate gate(budget, linkPollNowUs);
  // A response queued by the previous turn must not sit behind storage or
  // recording maintenance. Those cooperative phases may consume the whole
  // 2 ms slice; serving transport first prevents a valid record OK (and any
  // other terminal frame) from being starved until liveness recovery tears
  // down its still-active owner.
  if (txStepper_.active() && sessionActive_ && !quiesceRequested_ &&
      startupReady_ && transferPermitted()) {
    (void)gate.run([&]() { advanceTransmit(nowMs); });
    return;
  }
  if (!pollDeferredCleanup(gate)) return;
  if (recoverStalledLink(nowMs)) return;
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
    (void)gate.run([&]() { finishPendingManifestResponse(); });
    return;
  }
  if (manifestFailureRequestId_ != 0) {
    (void)gate.run([&]() { finishPendingManifestFailure(); });
    return;
  }
  if (!gate.run([&]() {
        recordingSession_.observeAutomaticStop(operation_, transactionGate_);
      })) return;
  if (!transferPermitted()) {
    (void)gate.run([&]() { disconnect(); });
    return;
  }
  if (txStepper_.active()) {
    (void)gate.run([&]() { advanceTransmit(nowMs); });
    return;
  }
  if (fileTransfer_.dataReady()) {
    (void)gate.run([&]() { fileTransfer_.advance(); });
    return;
  }
  if (manifestStepper_.active() || manifestStepper_.complete() ||
      manifestStepper_.failed()) {
    (void)gate.run([&]() { advanceManifest(nowMs); });
    return;
  }
  frameProcessedThisPoll_ = false;
  if (receivePhase_ == ReceivePhase::payload &&
      payloadUsed_ == currentHeader_.payloadLength) {
    processFrame(&gate);
    if (frameProcessedThisPoll_ || !gate.checkpoint()) return;
  }
  // A transport read is irreversible. If it consumed the previous poll's
  // last time unit, retain the byte until a fresh before-phase checkpoint.
  if (deferredRxByte_ >= 0) {
    if (!gate.checkpoint()) return;
    const uint8_t value = static_cast<uint8_t>(deferredRxByte_);
    deferredRxByte_ = -1;
    gate.consumeBytes();
    consumeByte(value, &gate);
    if (frameProcessedThisPoll_ || !gate.checkpoint()) return;
  }
  size_t sliceBytes = 0;
  while (gate.checkpoint() && !frameProcessedThisPoll_ &&
         !txStepper_.active() && transferPermitted() &&
         stream_->available() > 0) {
    const int value = stream_->read();
    if (value < 0) break;
    gate.consumeBytes();
    if (!gate.checkpoint()) {
      deferredRxByte_ = value;
      return;
    }
    consumeByte(static_cast<uint8_t>(value), &gate);
    if (++sliceBytes == kLinkReadSliceBytes) {
      sliceBytes = 0;
      if (!gate.checkpoint()) break;
    }
  }
  if (!gate.checkpoint()) return;
  // A request can start after the caller captured nowMs. Subtracting that
  // older timestamp from the freshly recorded byte time underflows uint32_t
  // and used to reject every upload immediately on some loop iterations.
  if (incomingKind_ != IncomingKind::none &&
      static_cast<uint32_t>(millis() - incomingLastByteMs_) > 5000) {
    if (!gate.run([&]() { failIncoming("binary transfer timed out"); })) {
      return;
    }
  }
}

void PokePodLinkService::consumeByte(uint8_t value, LinkPollPhaseGate *gate) {
  liveness_.noteProgress(millis());
  if (receivePhase_ == ReceivePhase::magic) {
    static constexpr uint8_t magic[4] = {'P', 'P', 'V', '2'};
    if (value == magic[magicMatched_]) {
      headerBytes_[magicMatched_++] = value;
      if (magicMatched_ == 4) {
        uint32_t observedUsbGeneration = 0;
        if (transport_ == LinkTransport::usb && usb_ != nullptr) {
          observedUsbGeneration = usb_->hostSessionSnapshot().generation;
          if (usbLinkMagicRequiresEpochReset(
                  sessionActive_, connectionGeneration_,
                  usbHostSessionGeneration_, observedUsbGeneration)) {
            // Retire the old logical owner without discarding bytes already
            // delivered for the new DTR epoch. disconnect() also clears the
            // old replay history and parser; the four magic bytes are restored
            // immediately below as the first bytes of the new session.
            disconnect();
          }
        }
        activateConnectionGeneration();
        if (observedUsbGeneration != 0) {
          usbHostSessionGeneration_ = observedUsbGeneration;
        }
        memcpy(headerBytes_, magic, sizeof(magic));
        magicMatched_ = sizeof(magic);
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
    if (currentHeader_.payloadLength == 0) processFrame(gate);
    return;
  }
  if (payload_ != nullptr && payloadUsed_ < kLinkMaxDataBytes) {
    payload_[payloadUsed_++] = value;
  }
  if (payloadUsed_ == currentHeader_.payloadLength) processFrame(gate);
}

void PokePodLinkService::resetFrame() {
  receivePhase_ = ReceivePhase::magic;
  headerUsed_ = 0;
  payloadUsed_ = 0;
  magicMatched_ = 0;
  currentHeader_ = LinkFrameHeader();
}

void PokePodLinkService::processFrame(LinkPollPhaseGate *gate) {
  if (gate != nullptr && !gate->checkpoint()) return;
  const LinkFrameHeader header = currentHeader_;
  const size_t size = payloadUsed_;
  if (!transferPermitted()) {
    resetFrame();
    return;
  }
  const bool valid = validateLinkPayload(header, payload_, size);
  if (gate != nullptr && !gate->checkpoint()) return;
  frameProcessedThisPoll_ = true;
  if (!valid) {
    resetFrame();
    sendError(header.requestId, "Link v2 CRC mismatch");
    return;
  }
  if (gate != nullptr && !gate->checkpoint()) {
    frameProcessedThisPoll_ = false;
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
  // Dispatch may consume the remainder of the slice. The shared poll gate
  // prevents timeout/reboot or any later lifecycle phase in this same poll.
  if (gate != nullptr) (void)gate->checkpoint();
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
  liveness_.noteProgress(millis());
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
  if (attempt.disposition == LinkWriteDisposition::progress &&
      attempt.bytes != 0) {
    liveness_.noteProgress(millis());
  }
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
    case LinkRecordingEventKind::startFailed:
      sendError(event.requestId, "recording start failed");
      break;
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

}  // namespace pokepod
