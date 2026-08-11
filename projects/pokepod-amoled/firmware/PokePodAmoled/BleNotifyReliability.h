#pragma once

#include <stdint.h>

namespace pokepod {

enum class BleNotifyInFlightState : uint8_t {
  idle,
  ready,
  awaitingHost,
  backoff,
  accepted,
  failed,
};

enum class BleNotifyFailure : uint8_t {
  none,
  rejected,
  timeout,
  cancelled,
};

struct BleNotifyInFlightSnapshot {
  BleNotifyInFlightState state = BleNotifyInFlightState::idle;
  BleNotifyFailure failure = BleNotifyFailure::none;
  uint32_t sequence = 0;
  uint8_t attempts = 0;
};

// Tracks host-queue acceptance for one BLE notification. It does not claim
// air delivery. Retrying never changes the voice frame sequence or v1 wire.
class BleNotifyInFlight {
 public:
  static constexpr uint8_t kMaxAttempts = 3;
  static constexpr uint32_t kRetryDelayMs = 8;
  static constexpr uint32_t kHostResolutionTimeoutMs = 40;
  static constexpr uint32_t kLifetimeMs = 160;

  bool start(uint32_t sequence, uint32_t nowMs) {
    if (active()) return false;
    state_ = BleNotifyInFlightState::ready;
    failure_ = BleNotifyFailure::none;
    sequence_ = sequence;
    attempts_ = 0;
    startedAtMs_ = nowMs;
    attemptAtMs_ = 0;
    retryAtMs_ = nowMs;
    return true;
  }

  bool canAttempt(uint32_t nowMs) const {
    return (state_ == BleNotifyInFlightState::ready ||
            state_ == BleNotifyInFlightState::backoff) &&
        reached(nowMs, retryAtMs_) && !lifetimeExpired(nowMs);
  }

  bool beginAttempt(uint32_t nowMs) {
    if (!canAttempt(nowMs)) return false;
    ++attempts_;
    attemptAtMs_ = nowMs;
    state_ = BleNotifyInFlightState::awaitingHost;
    return true;
  }

  bool resolve(bool acceptedByHost, uint32_t nowMs) {
    if (state_ != BleNotifyInFlightState::awaitingHost) return false;
    if (elapsed(nowMs, attemptAtMs_) >= kHostResolutionTimeoutMs) {
      scheduleFailure(BleNotifyFailure::timeout, nowMs);
      return false;
    }
    if (acceptedByHost) {
      state_ = BleNotifyInFlightState::accepted;
      failure_ = BleNotifyFailure::none;
      return true;
    }
    scheduleFailure(BleNotifyFailure::rejected, nowMs);
    return false;
  }

  void poll(uint32_t nowMs) {
    if (state_ == BleNotifyInFlightState::awaitingHost &&
        elapsed(nowMs, attemptAtMs_) >= kHostResolutionTimeoutMs) {
      scheduleFailure(BleNotifyFailure::timeout, nowMs);
    } else if ((state_ == BleNotifyInFlightState::ready ||
                state_ == BleNotifyInFlightState::backoff) &&
               lifetimeExpired(nowMs)) {
      state_ = BleNotifyInFlightState::failed;
      failure_ = BleNotifyFailure::timeout;
    }
  }

  void cancel() {
    if (!active()) return;
    state_ = BleNotifyInFlightState::failed;
    failure_ = BleNotifyFailure::cancelled;
  }

  void reset() { *this = BleNotifyInFlight(); }

  bool active() const {
    return state_ != BleNotifyInFlightState::idle &&
        state_ != BleNotifyInFlightState::accepted &&
        state_ != BleNotifyInFlightState::failed;
  }
  bool accepted() const {
    return state_ == BleNotifyInFlightState::accepted;
  }
  bool failed() const { return state_ == BleNotifyInFlightState::failed; }
  uint32_t sequence() const { return sequence_; }
  uint8_t attempts() const { return attempts_; }
  BleNotifyFailure failure() const { return failure_; }
  BleNotifyInFlightSnapshot snapshot() const {
    return {state_, failure_, sequence_, attempts_};
  }

 private:
  static uint32_t elapsed(uint32_t nowMs, uint32_t beforeMs) {
    return nowMs - beforeMs;
  }
  static bool reached(uint32_t nowMs, uint32_t deadlineMs) {
    return static_cast<int32_t>(nowMs - deadlineMs) >= 0;
  }
  bool lifetimeExpired(uint32_t nowMs) const {
    return elapsed(nowMs, startedAtMs_) >= kLifetimeMs;
  }
  void scheduleFailure(BleNotifyFailure failure, uint32_t nowMs) {
    failure_ = failure;
    if (attempts_ >= kMaxAttempts || lifetimeExpired(nowMs)) {
      state_ = BleNotifyInFlightState::failed;
      return;
    }
    state_ = BleNotifyInFlightState::backoff;
    retryAtMs_ = nowMs + kRetryDelayMs;
  }

  BleNotifyInFlightState state_ = BleNotifyInFlightState::idle;
  BleNotifyFailure failure_ = BleNotifyFailure::none;
  uint32_t sequence_ = 0;
  uint32_t startedAtMs_ = 0;
  uint32_t attemptAtMs_ = 0;
  uint32_t retryAtMs_ = 0;
  uint8_t attempts_ = 0;
};

}  // namespace pokepod
