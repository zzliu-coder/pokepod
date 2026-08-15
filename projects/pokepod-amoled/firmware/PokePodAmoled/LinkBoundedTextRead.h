#pragma once

#include <stddef.h>
#include <stdint.h>

namespace pokepod {

enum class LinkBoundedTextReadPhase : uint8_t {
  idle,
  reading,
  ready,
  failed,
  cancelled,
};

// Pure progress model for one fixed-capacity metadata slot. The FS adapter
// performs at most nextReadBytes() in a poll and reports the exact result.
class LinkBoundedTextRead {
 public:
  static constexpr size_t kMaximumBytes = 8192;
  static constexpr size_t kBytesPerPoll = 1024;

  bool begin(size_t bytes, size_t limit = kMaximumBytes) {
    reset();
    if (bytes == 0 || limit == 0 || limit > kMaximumBytes || bytes > limit) {
      phase_ = LinkBoundedTextReadPhase::failed;
      return false;
    }
    expected_ = bytes;
    phase_ = LinkBoundedTextReadPhase::reading;
    return true;
  }

  size_t nextReadBytes() const {
    if (phase_ != LinkBoundedTextReadPhase::reading || read_ >= expected_) {
      return 0;
    }
    const size_t remaining = expected_ - read_;
    return remaining < kBytesPerPoll ? remaining : kBytesPerPoll;
  }

  bool acceptRead(size_t bytes) {
    const size_t wanted = nextReadBytes();
    if (wanted == 0 || bytes == 0 || bytes > wanted) {
      fail();
      return false;
    }
    read_ += bytes;
    if (read_ == expected_) phase_ = LinkBoundedTextReadPhase::ready;
    return true;
  }

  void fail() {
    if (phase_ != LinkBoundedTextReadPhase::idle) {
      phase_ = LinkBoundedTextReadPhase::failed;
    }
  }

  void cancel() {
    if (phase_ != LinkBoundedTextReadPhase::idle) {
      phase_ = LinkBoundedTextReadPhase::cancelled;
    }
  }

  void reset() {
    phase_ = LinkBoundedTextReadPhase::idle;
    expected_ = 0;
    read_ = 0;
  }

  LinkBoundedTextReadPhase phase() const { return phase_; }
  bool active() const { return phase_ == LinkBoundedTextReadPhase::reading; }
  bool ready() const { return phase_ == LinkBoundedTextReadPhase::ready; }
  bool failed() const {
    return phase_ == LinkBoundedTextReadPhase::failed ||
        phase_ == LinkBoundedTextReadPhase::cancelled;
  }
  size_t expectedBytes() const { return expected_; }
  size_t bytesRead() const { return read_; }

 private:
  LinkBoundedTextReadPhase phase_ = LinkBoundedTextReadPhase::idle;
  size_t expected_ = 0;
  size_t read_ = 0;
};

}  // namespace pokepod
