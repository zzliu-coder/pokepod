#pragma once

#include <stdint.h>

namespace pokepod {

// This model is deliberately Arduino-free.  The firmware worker and host
// tests share the same vocabulary and transition rules without pulling the
// network stack into the test executable.
enum class TencentJobState : uint8_t {
  idle = 0,
  queued,
  working,
  cancelling,
  committing,
  succeeded,
  retryable,
  failed,
  cancelled,
  watchdog,
};

enum class TencentCancelReason : uint8_t {
  none = 0,
  user,
  maintenance,
  shutdown,
  storageRemoved,
  watchdog,
};

constexpr bool tencentJobOwnsResources(TencentJobState state) {
  return state == TencentJobState::queued ||
      state == TencentJobState::working ||
      state == TencentJobState::cancelling ||
      state == TencentJobState::committing ||
      state == TencentJobState::watchdog;
}

constexpr bool tencentJobIsTerminal(TencentJobState state) {
  return state == TencentJobState::succeeded ||
      state == TencentJobState::retryable ||
      state == TencentJobState::failed ||
      state == TencentJobState::cancelled;
}

constexpr bool tencentDeadlineReached(uint32_t nowMs, uint32_t deadlineMs) {
  return deadlineMs != 0 &&
      static_cast<int32_t>(nowMs - deadlineMs) >= 0;
}

constexpr const char *tencentJobStateName(TencentJobState state) {
  switch (state) {
    case TencentJobState::idle: return "idle";
    case TencentJobState::queued: return "queued";
    case TencentJobState::working: return "working";
    case TencentJobState::cancelling: return "cancelling";
    case TencentJobState::committing: return "committing";
    case TencentJobState::succeeded: return "succeeded";
    case TencentJobState::retryable: return "retryable";
    case TencentJobState::failed: return "failed";
    case TencentJobState::cancelled: return "cancelled";
    case TencentJobState::watchdog: return "watchdog";
  }
  return "unknown";
}

class TencentJobModel {
 public:
  bool queue(uint32_t generation, uint32_t deadlineMs) {
    if (generation == 0 || tencentJobOwnsResources(state_)) return false;
    generation_ = generation;
    deadlineMs_ = deadlineMs;
    cancelReason_ = TencentCancelReason::none;
    state_ = TencentJobState::queued;
    return true;
  }

  bool start(uint32_t generation) {
    if (!matches(generation) || state_ != TencentJobState::queued) return false;
    state_ = TencentJobState::working;
    return true;
  }

  bool cancel(uint32_t generation, TencentCancelReason reason) {
    if (matches(generation) &&
        (state_ == TencentJobState::cancelling ||
         state_ == TencentJobState::watchdog)) {
      return true;
    }
    if (!matches(generation) || !tencentJobOwnsResources(state_) ||
        state_ == TencentJobState::committing) {
      return false;
    }
    cancelReason_ = reason == TencentCancelReason::none
        ? TencentCancelReason::user : reason;
    state_ = reason == TencentCancelReason::watchdog
        ? TencentJobState::watchdog : TencentJobState::cancelling;
    return true;
  }

  bool checkWatchdog(uint32_t nowMs) {
    if ((state_ != TencentJobState::queued &&
         state_ != TencentJobState::working) ||
        !tencentDeadlineReached(nowMs, deadlineMs_)) {
      return false;
    }
    return cancel(generation_, TencentCancelReason::watchdog);
  }

  bool networkFinished(uint32_t generation, bool ok, bool transient) {
    if (!matches(generation) || !tencentJobOwnsResources(state_)) return false;
    if (cancelReason_ == TencentCancelReason::watchdog ||
        state_ == TencentJobState::watchdog) {
      state_ = TencentJobState::retryable;
    } else if (cancelReason_ != TencentCancelReason::none ||
               state_ == TencentJobState::cancelling) {
      state_ = TencentJobState::cancelled;
    } else if (ok) {
      state_ = TencentJobState::committing;
    } else {
      state_ = transient ? TencentJobState::retryable
                         : TencentJobState::failed;
    }
    return true;
  }

  bool commitFinished(uint32_t generation, bool committed) {
    if (!matches(generation) || state_ != TencentJobState::committing) {
      return false;
    }
    state_ = committed ? TencentJobState::succeeded
                       : TencentJobState::failed;
    return true;
  }

  TencentJobState state() const { return state_; }
  TencentCancelReason cancelReason() const { return cancelReason_; }
  uint32_t generation() const { return generation_; }
  uint32_t deadlineMs() const { return deadlineMs_; }

 private:
  bool matches(uint32_t generation) const {
    return generation != 0 && generation == generation_;
  }

  TencentJobState state_ = TencentJobState::idle;
  TencentCancelReason cancelReason_ = TencentCancelReason::none;
  uint32_t generation_ = 0;
  uint32_t deadlineMs_ = 0;
};

}  // namespace pokepod
