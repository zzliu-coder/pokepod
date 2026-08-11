#pragma once

#include <stddef.h>
#include <stdint.h>

namespace pokepod {

enum class LinkWriteDisposition : uint8_t {
  progress,
  wouldBlock,
  disconnected,
  failed,
};

struct LinkWriteAttempt {
  LinkWriteDisposition disposition = LinkWriteDisposition::wouldBlock;
  size_t bytes = 0;
};

class LinkWriteChannel {
 public:
  virtual ~LinkWriteChannel() = default;
  virtual LinkWriteAttempt writeSome(const uint8_t *data, size_t size) = 0;
};

enum class LinkTransferStepResult : uint8_t {
  idle,
  progress,
  wouldBlock,
  frameComplete,
  cancelled,
  disconnected,
  failed,
};

// Pure state used by both USB and TLS transfers. A caller may make exactly one
// write attempt for nextWriteBytes(), then reports the result through accept().
// The class never loops, sleeps or owns the source buffer.
class LinkTransferStepper {
 public:
  bool beginFrame(size_t bytes) {
    if (active_) return false;
    frameBytes_ = bytes;
    offset_ = 0;
    active_ = bytes > 0;
    return bytes > 0;
  }

  size_t nextWriteBytes(bool permitted, size_t maximumBytes) const {
    if (!active_ || !permitted || maximumBytes == 0) return 0;
    const size_t remaining = frameBytes_ - offset_;
    return remaining < maximumBytes ? remaining : maximumBytes;
  }

  LinkTransferStepResult accept(bool permittedAfter,
                                const LinkWriteAttempt &attempt) {
    if (!active_) return LinkTransferStepResult::idle;
    if (!permittedAfter) {
      cancel();
      return LinkTransferStepResult::cancelled;
    }
    if (attempt.disposition == LinkWriteDisposition::wouldBlock) {
      return LinkTransferStepResult::wouldBlock;
    }
    if (attempt.disposition == LinkWriteDisposition::disconnected) {
      cancel();
      return LinkTransferStepResult::disconnected;
    }
    if (attempt.disposition == LinkWriteDisposition::failed ||
        attempt.bytes == 0 || attempt.bytes > frameBytes_ - offset_) {
      cancel();
      return LinkTransferStepResult::failed;
    }
    offset_ += attempt.bytes;
    if (offset_ == frameBytes_) {
      active_ = false;
      return LinkTransferStepResult::frameComplete;
    }
    return LinkTransferStepResult::progress;
  }

  void cancel() {
    active_ = false;
    frameBytes_ = 0;
    offset_ = 0;
  }

  bool active() const { return active_; }
  size_t offset() const { return offset_; }
  size_t remaining() const {
    return active_ ? frameBytes_ - offset_ : 0;
  }

 private:
  bool active_ = false;
  size_t frameBytes_ = 0;
  size_t offset_ = 0;
};

}  // namespace pokepod
