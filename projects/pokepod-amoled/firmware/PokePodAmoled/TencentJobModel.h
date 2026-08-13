#pragma once

#include <atomic>
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
  return static_cast<int32_t>(nowMs - deadlineMs) >= 0;
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
    if (generation == 0 || tencentJobOwnsResources(state())) return false;
    generation_.store(generation, std::memory_order_relaxed);
    deadlineMs_.store(deadlineMs, std::memory_order_relaxed);
    cancelReason_.store(static_cast<uint8_t>(TencentCancelReason::none),
                        std::memory_order_relaxed);
    setState(TencentJobState::queued);
    return true;
  }

  bool start(uint32_t generation) {
    if (!matches(generation)) return false;
    uint8_t expected = static_cast<uint8_t>(TencentJobState::queued);
    return state_.compare_exchange_strong(
        expected, static_cast<uint8_t>(TencentJobState::working),
        std::memory_order_acq_rel);
  }

  bool cancel(uint32_t generation, TencentCancelReason reason) {
    if (!matches(generation)) return false;
    if (reason == TencentCancelReason::none) reason = TencentCancelReason::user;
    const TencentJobState target = reason == TencentCancelReason::watchdog
        ? TencentJobState::watchdog : TencentJobState::cancelling;
    for (;;) {
      const TencentJobState current = state();
      if (current == TencentJobState::cancelling ||
          current == TencentJobState::watchdog) {
        return true;
      }
      if ((current != TencentJobState::queued &&
           current != TencentJobState::working) ||
          current == TencentJobState::committing) {
        return false;
      }
      uint8_t expected = static_cast<uint8_t>(current);
      if (state_.compare_exchange_weak(
              expected, static_cast<uint8_t>(target),
              std::memory_order_acq_rel)) {
        cancelReason_.store(static_cast<uint8_t>(reason),
                            std::memory_order_release);
        return true;
      }
    }
  }

  bool checkWatchdog(uint32_t nowMs) {
    const TencentJobState current = state();
    if ((current != TencentJobState::queued &&
         current != TencentJobState::working) ||
        !tencentDeadlineReached(nowMs, deadlineMs())) {
      return false;
    }
    return cancel(generation(), TencentCancelReason::watchdog);
  }

  bool networkFinished(uint32_t generation, bool ok, bool transient) {
    const TencentJobState current = state();
    if (!matches(generation) ||
        (current != TencentJobState::queued &&
         current != TencentJobState::working &&
         current != TencentJobState::cancelling &&
         current != TencentJobState::watchdog)) {
      return false;
    }
    if (cancelReason() == TencentCancelReason::watchdog ||
        current == TencentJobState::watchdog) {
      setState(TencentJobState::retryable);
    } else if (cancelReason() != TencentCancelReason::none ||
               current == TencentJobState::cancelling) {
      setState(TencentJobState::cancelled);
    } else if (ok) {
      setState(TencentJobState::committing);
    } else {
      setState(transient ? TencentJobState::retryable
                         : TencentJobState::failed);
    }
    return true;
  }

  bool commitFinished(uint32_t generation, bool committed) {
    if (!matches(generation) || state() != TencentJobState::committing) {
      return false;
    }
    setState(committed ? TencentJobState::succeeded
                       : TencentJobState::failed);
    return true;
  }

  bool failWithoutActiveJob() {
    if (tencentJobOwnsResources(state())) return false;
    setState(TencentJobState::failed);
    return true;
  }

  bool resetIdle() {
    if (tencentJobOwnsResources(state())) return false;
    setState(TencentJobState::idle);
    return true;
  }

  // A reboot is a device-lifecycle boundary.  Once the network task has
  // published its result, reboot may deliberately abandon the not-yet-
  // committed capsule result; startup recovery will requeue the durable
  // transcribing record.  This transition touches only the in-memory job
  // model and never performs filesystem work.
  bool abandonForReboot(uint32_t generation) {
    if (!matches(generation) || !tencentJobOwnsResources(state())) return false;
    setState(TencentJobState::idle);
    return true;
  }

  TencentJobState state() const {
    return static_cast<TencentJobState>(
        state_.load(std::memory_order_acquire));
  }
  TencentCancelReason cancelReason() const {
    return static_cast<TencentCancelReason>(
        cancelReason_.load(std::memory_order_acquire));
  }
  uint32_t generation() const {
    return generation_.load(std::memory_order_acquire);
  }
  uint32_t deadlineMs() const {
    return deadlineMs_.load(std::memory_order_acquire);
  }
  bool matchesGeneration(uint32_t generation) const {
    return matches(generation);
  }

 private:
  void setState(TencentJobState state) {
    state_.store(static_cast<uint8_t>(state), std::memory_order_release);
  }

  bool matches(uint32_t generation) const {
    return generation != 0 && generation == this->generation();
  }

  std::atomic<uint8_t> state_{
      static_cast<uint8_t>(TencentJobState::idle)};
  std::atomic<uint8_t> cancelReason_{
      static_cast<uint8_t>(TencentCancelReason::none)};
  std::atomic<uint32_t> generation_{0};
  std::atomic<uint32_t> deadlineMs_{0};
};

}  // namespace pokepod
