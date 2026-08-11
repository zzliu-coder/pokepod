#pragma once

#include <stdint.h>

namespace pokepod {

enum class RecorderStartPollResult : uint8_t {
  idle = 0,
  pending,
  started,
  failed,
  cancelled,
};

// Main-loop owned policy for the storage-task start acknowledgement. The
// wireless transfer gate is observed before accepting an acknowledgement, so
// a result arriving exactly at the absolute deadline cannot publish success.
class RecorderStartState {
 public:
  static constexpr uint32_t kMaximumWaitMs = 5000;

  bool begin(uint32_t nowMs) {
    if (active_) return false;
    active_ = true;
    beganMs_ = nowMs;
    return true;
  }

  RecorderStartPollResult poll(uint32_t nowMs, bool gatePermitted,
                               bool acknowledged, bool succeeded) {
    if (!active_) return RecorderStartPollResult::idle;
    if (!gatePermitted ||
        static_cast<uint32_t>(nowMs - beganMs_) >= kMaximumWaitMs) {
      active_ = false;
      return RecorderStartPollResult::cancelled;
    }
    if (!acknowledged) return RecorderStartPollResult::pending;
    active_ = false;
    return succeeded ? RecorderStartPollResult::started
                     : RecorderStartPollResult::failed;
  }

  bool active() const { return active_; }

 private:
  uint32_t beganMs_ = 0;
  bool active_ = false;
};

}  // namespace pokepod
