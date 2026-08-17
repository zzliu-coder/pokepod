#include "LinkRecordingSession.h"

#include "AudioCaptureDispatcher.h"
#include "AudioCaptureRouter.h"
#include "AudioCaptureRuntime.h"
#include "AudioPipeline.h"
#include "BleVoiceService.h"
#include "CapsuleLibrary.h"
#include "LinkCapsuleTransactionGate.h"
#include "LinkTransferGate.h"
#include "WavRecorder.h"

namespace pokepod {

void LinkRecordingSession::begin(
    AudioPipeline &audio, AudioCaptureRuntime *captureRuntime,
    AudioCaptureDispatcher *captureDispatcher,
    AudioCaptureRouter &captureRouter, BleVoiceService &bleVoice,
    WavRecorder &recorder, CapsuleLibrary &library, Print &log) {
  audio_ = &audio;
  captureRuntime_ = captureRuntime;
  captureDispatcher_ = captureDispatcher;
  captureRouter_ = &captureRouter;
  bleVoice_ = &bleVoice;
  recorder_ = &recorder;
  library_ = &library;
  log_ = &log;
}


bool LinkRecordingSession::ready() const {
  return audio_ != nullptr && audio_->ready() && captureRuntime_ != nullptr &&
      captureRuntime_->ready() && captureDispatcher_ != nullptr &&
      captureRouter_ != nullptr && bleVoice_ != nullptr && recorder_ != nullptr &&
      library_ != nullptr && log_ != nullptr;
}

LinkRecordingRequestResult LinkRecordingSession::requestStart(
    uint32_t requestId, const String &capsuleId, uint32_t captureSessionId,
    const String &createdAt, RecorderOperationOwner recorderOwner,
    LinkOperation &operation, LinkCapsuleTransactionGate &transactionGate,
    LinkTransferGate *transferGate) {
  if (requestId == 0 || captureSessionId == 0 || capsuleId.isEmpty() ||
      audio_ == nullptr || captureRuntime_ == nullptr ||
      captureDispatcher_ == nullptr || captureRouter_ == nullptr ||
      bleVoice_ == nullptr || recorder_ == nullptr || library_ == nullptr ||
      log_ == nullptr) {
    return {LinkRecordingRequestStatus::failed,
            "recording session is unavailable"};
  }
  if (owned_ || start_.active() || stop_.active()) {
    return {LinkRecordingRequestStatus::busy, "recording is busy"};
  }

  // AudioCaptureRouter allows re-entry by one logical owner. A Link request
  // is a distinct capture session, so admission requires a genuinely idle
  // router before claiming localCapsule.
  const bool acquired = captureRouter_->available() &&
      captureRouter_->acquire(AudioCaptureOwner::localCapsule);
  if (!acquired) {
    return {LinkRecordingRequestStatus::busy, "recording is busy"};
  }

  transactionGate.beginOperation(transferGate);
  if (!start_.begin(requestId, captureSessionId)) {
    captureRouter_->release(AudioCaptureOwner::localCapsule);
    transactionGate.reset();
    return {LinkRecordingRequestStatus::failed, "recording start failed"};
  }
  owned_ = true;
  capturePrepareAttempted_ = false;
  capturePrepared_ = false;
  captureStopIssued_ = false;
  (void)operation.advance(LinkOperationState::processing);
  operation.ownResource(LinkOperationResource::router);
  operation.ownResource(LinkOperationResource::transaction);
  capsuleId_ = capsuleId;
  createdAt_ = createdAt;
  recorderOwner_ = recorderOwner;
  return {LinkRecordingRequestStatus::accepted, nullptr};
}

bool LinkRecordingSession::capturePreparePending() const {
  return owned_ && start_.active() && !start_.recorderRequested() &&
      !capturePrepareAttempted_;
}

void LinkRecordingSession::prepareCaptureOutsideLinkPoll() {
  if (!capturePreparePending()) return;
  capturePrepareAttempted_ = true;
  capturePrepared_ = audio_ != nullptr && captureRuntime_ != nullptr &&
      log_ != nullptr && captureRuntime_->prepare(*audio_, *log_);
}

bool LinkRecordingSession::captureStopPending() const {
  return owned_ && stop_.active() && !captureStopIssued_ &&
      captureRuntime_ != nullptr && captureRuntime_->running();
}

void LinkRecordingSession::stopCaptureOutsideLinkPoll() {
  if (!captureStopPending() || log_ == nullptr) return;
  captureStopIssued_ = true;
  // A bounded I2S read may still be in flight.  stop() may therefore return
  // false while leaving a finalize token for later cooperative polling.
  (void)captureRuntime_->stop(*log_);
}

bool LinkRecordingSession::requestStop(
    uint32_t requestId, bool commit, bool respond,
    bool operationOwnsRequest, LinkOperation &operation,
    LinkCapsuleTransactionGate &transactionGate) {
  (void)transactionGate;
  if (!owned_ || captureRuntime_ == nullptr || recorder_ == nullptr ||
      captureRouter_ == nullptr || log_ == nullptr) {
    return false;
  }
  if (stop_.active()) {
    if (!respond) stop_.suppressResponseAndAbort();
    return false;
  }
  if (!stop_.begin(requestId, commit, respond)) return false;
  if (requestId != 0 && operationOwnsRequest) {
    (void)operation.advance(LinkOperationState::processing);
    if (ownsTransferredResources()) {
      operation.ownResource(LinkOperationResource::recordingSession);
      stopOperationTracksSession_ = true;
    } else {
      operation.ownResource(LinkOperationResource::router);
      operation.ownResource(LinkOperationResource::transaction);
    }
  }
  captureStopIssued_ = false;
  // The App loop issues the potentially blocking capture stop outside Link's
  // 2 ms budget. Ownership remains here until both capture and recorder reach
  // terminal state.
  return true;
}

LinkRecordingEvent LinkRecordingSession::advanceStart(
    LinkOperation &operation, LinkCapsuleTransactionGate &transactionGate,
    LinkTransport transport, LinkTransferGate *transferGate,
    bool sessionActive, bool quiesceRequested) {
  if (!start_.active() || !owned_ || recorder_ == nullptr ||
      captureRuntime_ == nullptr || captureRouter_ == nullptr ||
      audio_ == nullptr || log_ == nullptr) {
    return {};
  }
  CapsuleTransactionGate *gate =
      transport == LinkTransport::wifi || !sessionActive || quiesceRequested
          ? &transactionGate
          : nullptr;
  const uint32_t requestId = start_.requestId();
  const uint32_t captureSessionId = start_.captureSessionId();
  const bool transportAlive = sessionActive && !quiesceRequested &&
      linkTransferPermitted(transferGate, millis());

  if (!start_.recorderRequested()) {
    // App owns the potentially blocking I2S/codec prepare phase. Link waits
    // across turns until that external phase publishes its result.
    if (transportAlive && !capturePrepareAttempted_) return {};
    const bool accepted = transportAlive && capturePrepared_ &&
        recorder_->requestStart(
        *log_, capsuleId_, createdAt_, recorderOwner_);
    if (accepted) {
      (void)start_.markRecorderRequested();
      capturePrepareAttempted_ = false;
      capturePrepared_ = false;
      return {};
    }
    if (recorder_->ownedBy(recorderOwner_) || recorder_->operationActive() ||
        recorder_->terminalResult().pending()) {
      (void)start_.markRecorderRequested();
      start_.finish();
      capturePrepareAttempted_ = false;
      capturePrepared_ = false;
      capsuleId_ = "";
      createdAt_ = "";
      recorderOwner_ = RecorderOperationOwner::none;
      (void)requestStop(requestId, false, transportAlive, true, operation,
                        transactionGate);
      return {};
    }
    start_.finish();
    capturePrepareAttempted_ = false;
    capturePrepared_ = false;
    capsuleId_ = "";
    createdAt_ = "";
    recorderOwner_ = RecorderOperationOwner::none;
    captureRouter_->release(AudioCaptureOwner::localCapsule);
    transactionGate.reset();
    owned_ = false;
    operation.releaseResource(LinkOperationResource::router);
    operation.releaseResource(LinkOperationResource::transaction);
    if (!transportAlive && operation.active()) {
      operation.cancel(quiesceRequested
          ? LinkOperationCancelReason::quiesce
          : LinkOperationCancelReason::deadline);
    }
    LinkRecordingEvent event;
    event.kind = LinkRecordingEventKind::startFailed;
    event.requestId = requestId;
    event.respond = transportAlive;
    return event;
  }
  const RecorderStartPollResult result = recorder_->pollStart(
      *log_, millis(), gate);
  if (result == RecorderStartPollResult::pending) return {};

  const String capsuleId = capsuleId_;
  start_.finish();
  capturePrepareAttempted_ = false;
  capturePrepared_ = false;
  captureStopIssued_ = false;
  capsuleId_ = "";
  createdAt_ = "";
  recorderOwner_ = RecorderOperationOwner::none;

  if (result == RecorderStartPollResult::started && transportAlive &&
      captureRuntime_->startPrepared(*audio_, captureSessionId, *log_)) {
    if (!operation.transferResourcesToRecordingSession()) {
      (void)requestStop(requestId, false, transportAlive, true, operation,
                        transactionGate);
      return {};
    }
    routerOwned_ = true;
    transactionOwned_ = true;
    LinkRecordingEvent event;
    event.kind = LinkRecordingEventKind::startReady;
    event.requestId = requestId;
    event.capsuleId = capsuleId;
    event.respond = true;
    return event;
  }

  if (stop_.active()) {
    stop_.suppressResponseAndAbort();
  } else {
    const bool operationOwnsRequest = transportAlive &&
        operation.active() && operation.requestId() == requestId;
    (void)requestStop(requestId, false, transportAlive,
                      operationOwnsRequest, operation, transactionGate);
  }
  return {};
}

LinkRecordingEvent LinkRecordingSession::advanceStop(
    LinkOperation &operation, LinkCapsuleTransactionGate &transactionGate,
    LinkTransport transport, LinkTransferGate *transferGate,
    bool sessionActive, bool quiesceRequested) {
  if (!owned_ || captureRuntime_ == nullptr || recorder_ == nullptr ||
      captureRouter_ == nullptr || log_ == nullptr) {
    return {};
  }

  CapsuleTransactionGate *recordingGate =
      transport == LinkTransport::wifi || !sessionActive || quiesceRequested
          ? &transactionGate
          : nullptr;
  // Polling while capture is active lets the recorder storage task advance
  // periodic checkpoints without granting App ownership of a Link session.
  (void)recorder_->pollFinalize(*log_, millis(), recordingGate);
  if (start_.active()) return {};
  if (!stop_.active() && !recorder_->operationActive() &&
      recorder_->terminalResult().pending()) {
    (void)requestStop(0, false, false, false, operation, transactionGate);
  }
  if (!stop_.active()) return {};

  if (stop_.awaitsCapture()) {
    // App polls the capture terminal before every Link/UI early return. Link
    // observes only the published state and never finalizes hardware here.
    if (captureRuntime_->running()) return {};
    AudioCaptureDispatchResult dispatch;
    if (captureDispatcher_ != nullptr && audio_ != nullptr &&
        bleVoice_ != nullptr) {
      dispatch = captureDispatcher_->drain(
          *captureRuntime_, *captureRouter_, *audio_, *recorder_, *bleVoice_,
          *log_, millis());
    } else {
      dispatch.ok = false;
      dispatch.routingFailure = true;
    }
    const bool complete = stop_.commitRequested() && dispatch.ok &&
        !captureRuntime_->incomplete() &&
        !recorder_->captureFailureLatched();
    const AudioCaptureFrontEndSnapshot finalMetrics =
        captureRuntime_->frontEndSnapshot();
    recorder_->observeAudioMetrics(finalMetrics.sessionId,
                                   finalMetrics.generation,
                                   finalMetrics.asMetrics());
    recorder_->observeCaptureTelemetry(
        captureRuntime_->metrics(), captureDispatcher_->metrics(),
        static_cast<uint32_t>(captureRuntime_->taskStackHighWater()));
    if (recorder_->recording()) {
      if (complete) {
        (void)recorder_->stop(*log_, recorder_->stopRequested()
            ? recorder_->requestedStopReason()
            : RecorderStopReason::user);
      } else {
        (void)recorder_->abortCapture(*log_);
      }
    }
    stop_.captureFinalized();
  }

  if (!stop_.awaitsRecorder()) return {};
  (void)recorder_->pollFinalize(*log_, millis(), recordingGate);
  if (recorder_->operationActive()) return {};

  RecorderOutcome outcome;
  if (!recorder_->takeTerminalResult(outcome)) return {};

  const uint32_t requestId = stop_.requestId();
  const bool respond = stop_.shouldRespond() && sessionActive &&
      linkTransferPermitted(transferGate, millis());
  const bool committed = outcome.success();
  captureRouter_->release(AudioCaptureOwner::localCapsule);
  routerOwned_ = false;
  owned_ = false;
  transactionGate.reset();
  transactionOwned_ = false;
  if (stopOperationTracksSession_) {
    operation.releaseResource(LinkOperationResource::recordingSession);
  } else {
    operation.releaseResource(LinkOperationResource::router);
    operation.releaseResource(LinkOperationResource::transaction);
  }
  stopOperationTracksSession_ = false;
  captureStopIssued_ = false;
  stop_.finish();

  LinkRecordingEvent event;
  event.requestId = requestId;
  event.respond = respond && requestId != 0;
  if (!event.respond) return event;
  if (!committed) {
    event.kind = LinkRecordingEventKind::stopFailed;
  } else if (library_ != nullptr && library_->requestScan()) {
    event.kind = LinkRecordingEventKind::stopCommitted;
  } else {
    event.kind = LinkRecordingEventKind::stopCommittedIndexFailed;
  }
  return event;
}

LinkRecordingEvent LinkRecordingSession::poll(
    LinkOperation &operation, LinkCapsuleTransactionGate &transactionGate,
    LinkTransport transport, LinkTransferGate *transferGate,
    bool sessionActive, bool quiesceRequested) {
  LinkRecordingEvent event = advanceStart(
      operation, transactionGate, transport, transferGate, sessionActive,
      quiesceRequested);
  if (event.kind != LinkRecordingEventKind::none) return event;
  return advanceStop(operation, transactionGate, transport, transferGate,
                     sessionActive, quiesceRequested);
}

void LinkRecordingSession::observeAutomaticStop(
    LinkOperation &operation, LinkCapsuleTransactionGate &transactionGate) {
  if (!owned_ || stop_.active() || recorder_ == nullptr) return;
  if (recorder_->captureFailureLatched()) {
    (void)requestStop(0, false, false, false, operation, transactionGate);
  } else if (recorder_->stopRequested()) {
    (void)requestStop(0, true, false, false, operation, transactionGate);
  }
}

void LinkRecordingSession::disconnect(
    LinkOperation &operation, LinkCapsuleTransactionGate &transactionGate) {
  if (owned_ && start_.active() && !start_.recorderRequested()) {
    start_.finish();
    capturePrepareAttempted_ = false;
    capturePrepared_ = false;
    captureStopIssued_ = false;
    capsuleId_ = "";
    createdAt_ = "";
    recorderOwner_ = RecorderOperationOwner::none;
    captureRouter_->release(AudioCaptureOwner::localCapsule);
    transactionGate.reset();
    operation.releaseResource(LinkOperationResource::router);
    operation.releaseResource(LinkOperationResource::transaction);
    owned_ = false;
    return;
  }
  if (stop_.active()) {
    stop_.suppressResponseAndAbort();
  } else if (owned_) {
    (void)requestStop(0, false, false, false, operation, transactionGate);
  }
  if (owned_) transactionGate.cancel();
}

bool LinkRecordingSession::recordingActive() const {
  return owned_ && recorder_ != nullptr && recorder_->recording();
}

}  // namespace pokepod
