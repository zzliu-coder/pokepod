#pragma once

#include <stdint.h>

namespace pokepod {

// Owns the Link request while the recorder storage task is asynchronously
// admitting a new session.  A duplicate frame with the same request id is
// coalesced into the original request; completion belongs to the real storage
// ACK/cancellation terminal, not to the dispatch turn that accepted it.
class LinkRecordingStart {
 public:
  bool begin(uint32_t requestId, uint32_t captureSessionId) {
    if (active_ || requestId == 0 || captureSessionId == 0) return false;
    active_ = true;
    requestId_ = requestId;
    captureSessionId_ = captureSessionId;
    return true;
  }

  bool active() const { return active_; }
  bool ownsRequest(uint32_t requestId) const {
    return active_ && requestId_ == requestId;
  }
  uint32_t requestId() const { return requestId_; }
  uint32_t captureSessionId() const { return captureSessionId_; }

  void finish() {
    active_ = false;
    requestId_ = 0;
    captureSessionId_ = 0;
  }

 private:
  bool active_ = false;
  uint32_t requestId_ = 0;
  uint32_t captureSessionId_ = 0;
};

}  // namespace pokepod
