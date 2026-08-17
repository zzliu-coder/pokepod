#pragma once

#include <stdint.h>

namespace pokepod {

// Owns the Link request while the recorder storage task is asynchronously
// admitting a new session.  A duplicate frame with the same request id is
// coalesced into the original request; completion belongs to the real storage
// ACK/cancellation terminal, not to the dispatch turn that accepted it.
class LinkRecordingStart {
 public:
  static constexpr uint32_t kPrepareTimeoutMs = 8000;

  bool begin(uint32_t requestId, uint32_t captureSessionId,
             uint32_t nowMs = 0) {
    if (active_ || requestId == 0 || captureSessionId == 0) return false;
    active_ = true;
    requestId_ = requestId;
    captureSessionId_ = captureSessionId;
    startedAtMs_ = nowMs;
    return true;
  }

  bool active() const { return active_; }
  bool ownsRequest(uint32_t requestId) const {
    return active_ && requestId_ == requestId;
  }
  uint32_t requestId() const { return requestId_; }
  uint32_t captureSessionId() const { return captureSessionId_; }
  bool recorderRequested() const { return recorderRequested_; }
  bool prepareDeadlineReached(uint32_t nowMs) const {
    return active_ && !recorderRequested_ &&
        static_cast<uint32_t>(nowMs - startedAtMs_) >= kPrepareTimeoutMs;
  }
  bool markRecorderRequested() {
    if (!active_ || recorderRequested_) return false;
    recorderRequested_ = true;
    return true;
  }

  void finish() {
    active_ = false;
    requestId_ = 0;
    captureSessionId_ = 0;
    startedAtMs_ = 0;
    recorderRequested_ = false;
  }

 private:
  bool active_ = false;
  uint32_t requestId_ = 0;
  uint32_t captureSessionId_ = 0;
  uint32_t startedAtMs_ = 0;
  bool recorderRequested_ = false;
};

}  // namespace pokepod
