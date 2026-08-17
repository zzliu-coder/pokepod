#pragma once

#include <Arduino.h>
#include <stdint.h>

#include "LinkOperation.h"
#include "LinkRecordingStart.h"
#include "LinkRecordingStop.h"
#include "RecorderOutcome.h"

namespace pokepod {

class AudioCaptureDispatcher;
class AudioCaptureRouter;
class AudioCaptureRuntime;
class AudioPipeline;
class BleVoiceService;
class CapsuleLibrary;
class LinkCapsuleTransactionGate;
class LinkTransferGate;
class WavRecorder;

enum class LinkRecordingRequestStatus : uint8_t {
  accepted = 0,
  busy,
  failed,
  cleanupPending,
};

struct LinkRecordingRequestResult {
  LinkRecordingRequestStatus status = LinkRecordingRequestStatus::failed;
  const char *message = "recording request failed";
};

enum class LinkRecordingEventKind : uint8_t {
  none = 0,
  startReady,
  startFailed,
  stopCommitted,
  stopCommittedIndexFailed,
  stopFailed,
};

struct LinkRecordingEvent {
  LinkRecordingEventKind kind = LinkRecordingEventKind::none;
  uint32_t requestId = 0;
  String capsuleId;
  bool respond = false;
};

// Owns the complete Link recording session after request dispatch.  It is the
// only component allowed to combine LinkRecordingStart/Stop, the capture
// runtime, recorder terminal facts, router ownership and transaction-gate
// cancellation.  Wire responses remain in PokePodLinkService.
class LinkRecordingSession {
 public:
  void begin(AudioPipeline &audio, AudioCaptureRuntime *captureRuntime,
             AudioCaptureDispatcher *captureDispatcher,
             AudioCaptureRouter &captureRouter, BleVoiceService &bleVoice,
             WavRecorder &recorder, CapsuleLibrary &library, Print &log);

  LinkRecordingRequestResult requestStart(
      uint32_t requestId, const String &capsuleId, uint32_t captureSessionId,
      const String &createdAt, RecorderOperationOwner recorderOwner,
      LinkOperation &operation, LinkCapsuleTransactionGate &transactionGate,
      LinkTransferGate *transferGate);
  bool requestStop(uint32_t requestId, bool commit, bool respond,
                   bool operationOwnsRequest, LinkOperation &operation,
                   LinkCapsuleTransactionGate &transactionGate);

  // Audio hardware allocation is intentionally driven by the App loop, never
  // from the Link 2 ms poll budget.  The Link poll only observes this result
  // and advances the asynchronous recorder state machine.
  bool capturePreparePending() const;
  bool capturePrepareAttempted() const { return capturePrepareAttempted_; }
  bool capturePrepared() const { return capturePrepared_; }
  void prepareCaptureOutsideLinkPoll();
  bool captureStopPending() const;
  bool captureStopIssued() const { return captureStopIssued_; }
  void stopCaptureOutsideLinkPoll();

  LinkRecordingEvent poll(LinkOperation &operation,
                          LinkCapsuleTransactionGate &transactionGate,
                          LinkTransport transport,
                          LinkTransferGate *transferGate,
                          bool sessionActive, bool quiesceRequested);
  void observeAutomaticStop(LinkOperation &operation,
                            LinkCapsuleTransactionGate &transactionGate);
  void disconnect(LinkOperation &operation,
                  LinkCapsuleTransactionGate &transactionGate);

  bool ready() const;
  bool ownsRequest(uint32_t requestId) const {
    return start_.ownsRequest(requestId) || stop_.ownsRequest(requestId);
  }
  bool owned() const { return owned_; }
  bool ownsTransferredResources() const {
    return routerOwned_ && transactionOwned_;
  }
  bool stopActive() const { return stop_.active(); }
  bool recordingActive() const;
  bool quiesced() const {
    return !owned_ && !start_.active() && !stop_.active();
  }

 private:
  LinkRecordingEvent advanceStart(
      LinkOperation &operation, LinkCapsuleTransactionGate &transactionGate,
      LinkTransport transport, LinkTransferGate *transferGate,
      bool sessionActive, bool quiesceRequested);
  LinkRecordingEvent advanceStop(
      LinkOperation &operation, LinkCapsuleTransactionGate &transactionGate,
      LinkTransport transport, LinkTransferGate *transferGate,
      bool sessionActive, bool quiesceRequested);

  AudioPipeline *audio_ = nullptr;
  AudioCaptureRuntime *captureRuntime_ = nullptr;
  AudioCaptureDispatcher *captureDispatcher_ = nullptr;
  AudioCaptureRouter *captureRouter_ = nullptr;
  BleVoiceService *bleVoice_ = nullptr;
  WavRecorder *recorder_ = nullptr;
  CapsuleLibrary *library_ = nullptr;
  Print *log_ = nullptr;

  bool owned_ = false;
  bool routerOwned_ = false;
  bool transactionOwned_ = false;
  bool stopOperationTracksSession_ = false;
  bool capturePrepareAttempted_ = false;
  bool capturePrepared_ = false;
  bool captureStopIssued_ = false;
  LinkRecordingStart start_;
  String capsuleId_;
  String createdAt_;
  RecorderOperationOwner recorderOwner_ = RecorderOperationOwner::none;
  LinkRecordingStop stop_;
};

}  // namespace pokepod
