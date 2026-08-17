#pragma once

#include <stdint.h>

namespace pokepod {

enum class LinkLivenessStall : uint8_t {
  none = 0,
  receive = 1,
  transmit = 2,
  operation = 3,
};

struct LinkLivenessSnapshot {
  uint32_t generation = 0;
  uint32_t lastProgressMs = 0;
  uint32_t recoveryCount = 0;
  uint32_t lastRecoveryMs = 0;
  LinkLivenessStall lastStall = LinkLivenessStall::none;
};

// Pure monotonic policy for the USB/Wi-Fi Link control plane. Long-running
// recording/storage owners remain outside this policy; the production adapter
// only marks an operation recoverable after all operational resources drain.
class LinkLivenessProbe {
 public:
  static constexpr uint32_t kStallTimeoutMs = 5000;

  void reset() {
    snapshot_ = {};
    armed_ = false;
  }

  void openSession(uint32_t generation, uint32_t nowMs) {
    if (generation == 0) return;
    snapshot_.generation = generation;
    snapshot_.lastProgressMs = nowMs;
    armed_ = true;
  }

  void noteProgress(uint32_t nowMs) {
    snapshot_.lastProgressMs = nowMs;
    armed_ = true;
  }

  LinkLivenessStall observe(uint32_t nowMs, bool sessionActive,
                            uint32_t requestId, uint32_t queuedFrames,
                            bool receivePartial, bool transmitPending,
                            bool operationRecoverable) const {
    if (!sessionActive || !armed_ ||
        static_cast<uint32_t>(nowMs - snapshot_.lastProgressMs) <
            kStallTimeoutMs) {
      return LinkLivenessStall::none;
    }
    if (receivePartial) return LinkLivenessStall::receive;
    if (transmitPending || queuedFrames != 0) {
      return operationRecoverable ? LinkLivenessStall::transmit
                                  : LinkLivenessStall::none;
    }
    if (requestId != 0 && operationRecoverable) {
      return LinkLivenessStall::operation;
    }
    return LinkLivenessStall::none;
  }

  void recovered(LinkLivenessStall stall, uint32_t nowMs) {
    if (stall == LinkLivenessStall::none) return;
    snapshot_.lastStall = stall;
    snapshot_.lastRecoveryMs = nowMs;
    ++snapshot_.recoveryCount;
    if (snapshot_.recoveryCount == 0) snapshot_.recoveryCount = 1;
    snapshot_.lastProgressMs = nowMs;
  }

  const LinkLivenessSnapshot &snapshot() const { return snapshot_; }

 private:
  LinkLivenessSnapshot snapshot_{};
  bool armed_ = false;
};

}  // namespace pokepod
