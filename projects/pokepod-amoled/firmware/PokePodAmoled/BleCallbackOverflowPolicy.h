#pragma once

#include "BleVoiceCallbackMailbox.h"

namespace pokepod {

enum class BleCallbackOverflowPhase : uint8_t {
  idle,
  disconnecting,
  confirmed,
  hardFailed,
};

struct BleCallbackOverflowActions {
  bool disconnect = false;
  bool enteredHardFailed = false;
};

struct BleSleepQuiescenceFacts {
  bool sessionActive = false;
  bool pairingActive = false;
  bool enableTransitionPending = false;
  bool overflowCleanupActive = false;
  bool physicalConnectionPending = false;
  bool notifyPending = false;
  bool callbackMailboxEmpty = true;
  bool notifyMailboxEmpty = true;
};

inline bool bleVoiceQuiescedForSleep(
    const BleSleepQuiescenceFacts &facts) {
  return !facts.sessionActive && !facts.pairingActive &&
      !facts.enableTransitionPending && !facts.overflowCleanupActive &&
      !facts.physicalConnectionPending && !facts.notifyPending &&
      facts.callbackMailboxEmpty && facts.notifyMailboxEmpty;
}

// Owns the complete fail-closed lifetime of one callback-mailbox overflow.
// The frozen controller epoch, retry cadence and deadline are independent of
// the user's persistent enable/disable intent. Only an exact physical
// disconnect can move the epoch to confirmed and authorize mailbox rearming.
class BleCallbackOverflowPolicy {
 public:
  static constexpr uint32_t kDisconnectRetryMs = 250;
  static constexpr uint32_t kCleanupDeadlineMs = 5000;

  bool begin(const BleVoiceConnectionEpoch &epoch, uint32_t nowMs) {
    if (active()) return false;
    epoch_ = epoch;
    startedAtMs_ = nowMs;
    nextRetryAtMs_ = nowMs;
    deadlineMs_ = nowMs + kCleanupDeadlineMs;
    attempts_ = 0;
    phase_ = epoch.valid() ? BleCallbackOverflowPhase::disconnecting
                           : BleCallbackOverflowPhase::hardFailed;
    if (phase_ == BleCallbackOverflowPhase::hardFailed) {
      ++hardFailureCount_;
    }
    return true;
  }

  bool active() const { return phase_ != BleCallbackOverflowPhase::idle; }
  bool hardFailed() const {
    return phase_ == BleCallbackOverflowPhase::hardFailed;
  }
  bool requiresProcessRecovery() const {
    return phase_ == BleCallbackOverflowPhase::hardFailed && epoch_.valid();
  }
  // An overflow without a trustworthy physical epoch cannot be confirmed
  // against a controller connection. It is safe to clear that failed
  // admission after the service has stopped accepting callback events.
  bool recoverInvalidEpoch() {
    if (phase_ != BleCallbackOverflowPhase::hardFailed || epoch_.valid()) {
      return false;
    }
    clearState();
    return true;
  }
  bool physicalConnectionPending() const {
    return active() && epoch_.valid();
  }
  BleCallbackOverflowPhase phase() const { return phase_; }
  const BleVoiceConnectionEpoch &epoch() const { return epoch_; }
  uint32_t startedAtMs() const { return startedAtMs_; }
  uint32_t nextRetryAtMs() const { return nextRetryAtMs_; }
  uint32_t deadlineMs() const { return deadlineMs_; }
  uint16_t attempts() const { return attempts_; }
  uint32_t hardFailureCount() const { return hardFailureCount_; }

  BleCallbackOverflowActions poll(uint32_t nowMs) {
    BleCallbackOverflowActions actions;
    if (phase_ != BleCallbackOverflowPhase::disconnecting) return actions;
    if (reached(nowMs, deadlineMs_)) {
      phase_ = BleCallbackOverflowPhase::hardFailed;
      ++hardFailureCount_;
      actions.enteredHardFailed = true;
      return actions;
    }
    if (reached(nowMs, nextRetryAtMs_)) {
      ++attempts_;
      nextRetryAtMs_ = nowMs + kDisconnectRetryMs;
      actions.disconnect = true;
    }
    return actions;
  }

  bool confirm(const BleVoiceConnectionEpoch &physicalDisconnect) {
    const bool waitingForDisconnect =
        phase_ == BleCallbackOverflowPhase::disconnecting ||
        (phase_ == BleCallbackOverflowPhase::hardFailed && epoch_.valid());
    if (!waitingForDisconnect ||
        !physicalDisconnect.matches(epoch_)) {
      return false;
    }
    phase_ = BleCallbackOverflowPhase::confirmed;
    return true;
  }

  BleVoiceConnectionEpoch finish() {
    if (phase_ != BleCallbackOverflowPhase::confirmed) return {};
    const BleVoiceConnectionEpoch closed = epoch_;
    clearState();
    return closed;
  }

 private:
  void clearState() {
    phase_ = BleCallbackOverflowPhase::idle;
    epoch_ = {};
    startedAtMs_ = 0;
    nextRetryAtMs_ = 0;
    deadlineMs_ = 0;
    attempts_ = 0;
  }

  static bool reached(uint32_t nowMs, uint32_t deadlineMs) {
    return static_cast<int32_t>(nowMs - deadlineMs) >= 0;
  }

  BleCallbackOverflowPhase phase_ = BleCallbackOverflowPhase::idle;
  BleVoiceConnectionEpoch epoch_;
  uint32_t startedAtMs_ = 0;
  uint32_t nextRetryAtMs_ = 0;
  uint32_t deadlineMs_ = 0;
  uint16_t attempts_ = 0;
  uint32_t hardFailureCount_ = 0;
};

}  // namespace pokepod
