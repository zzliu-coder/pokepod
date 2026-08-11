#pragma once

#include <stdint.h>

namespace pokepod {

enum class LinkRecordingStopPhase : uint8_t {
  idle = 0,
  awaitCaptureFinalize,
  awaitRecorderCleanup,
};

// Records ownership while the realtime capture task and recorder settle.  A
// stop timeout is asynchronous: the caller must retain every capture resource
// until both layers have reached a terminal state.
class LinkRecordingStop {
 public:
  bool begin(uint32_t requestId, bool commit, bool respond) {
    if (active()) return false;
    phase_ = LinkRecordingStopPhase::awaitCaptureFinalize;
    requestId_ = requestId;
    commit_ = commit;
    respond_ = respond;
    recorderSucceeded_ = false;
    return true;
  }

  bool active() const { return phase_ != LinkRecordingStopPhase::idle; }
  bool awaitsCapture() const {
    return phase_ == LinkRecordingStopPhase::awaitCaptureFinalize;
  }
  bool awaitsRecorder() const {
    return phase_ == LinkRecordingStopPhase::awaitRecorderCleanup;
  }
  bool ownsRequest(uint32_t requestId) const {
    return active() && requestId_ != 0 && requestId_ == requestId;
  }
  uint32_t requestId() const { return requestId_; }
  bool commitRequested() const { return commit_; }
  bool shouldRespond() const { return respond_; }
  bool recorderSucceeded() const { return recorderSucceeded_; }

  void suppressResponseAndAbort() {
    commit_ = false;
    respond_ = false;
  }

  void captureFinalized(bool recorderSucceeded) {
    if (!awaitsCapture()) return;
    recorderSucceeded_ = recorderSucceeded;
    phase_ = LinkRecordingStopPhase::awaitRecorderCleanup;
  }

  void finish() {
    phase_ = LinkRecordingStopPhase::idle;
    requestId_ = 0;
    commit_ = false;
    respond_ = false;
    recorderSucceeded_ = false;
  }

 private:
  LinkRecordingStopPhase phase_ = LinkRecordingStopPhase::idle;
  uint32_t requestId_ = 0;
  bool commit_ = false;
  bool respond_ = false;
  bool recorderSucceeded_ = false;
};

}  // namespace pokepod
