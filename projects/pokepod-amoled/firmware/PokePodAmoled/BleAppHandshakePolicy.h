#pragma once

#include <cstdint>

namespace pokepod {

class BleAppHandshakePolicy {
 public:
  static constexpr uint32_t kTimeoutMs = 15000;
  static constexpr uint32_t kRetryMs = 250;
  static constexpr uint32_t kCleanupDeadlineMs = 5000;

  void connected(uint32_t nowMs) {
    active_ = true;
    ready_ = false;
    connectedAtMs_ = nowMs;
    lastDisconnectRequestAtMs_ = 0;
    disconnectStartedAtMs_ = 0;
    disconnectPending_ = false;
    hardFailed_ = false;
  }

  void ready() {
    if (!disconnectPending_) ready_ = true;
  }

  void disconnected() {
    active_ = false;
    ready_ = false;
    connectedAtMs_ = 0;
    lastDisconnectRequestAtMs_ = 0;
    disconnectStartedAtMs_ = 0;
    disconnectPending_ = false;
    hardFailed_ = false;
  }

  bool disconnectPending() const { return disconnectPending_; }
  bool recoveryRequired() const { return hardFailed_; }

  bool requestDisconnect(uint32_t nowMs, bool pairingActive) {
    if (!active_ || hardFailed_ ||
        (!disconnectPending_ &&
         (ready_ || pairingActive || nowMs - connectedAtMs_ < kTimeoutMs))) {
      return false;
    }
    if (!disconnectPending_) {
      disconnectPending_ = true;
      disconnectStartedAtMs_ = nowMs;
    } else if (nowMs - disconnectStartedAtMs_ >= kCleanupDeadlineMs) {
      hardFailed_ = true;
      return false;
    }
    if (lastDisconnectRequestAtMs_ != 0 &&
        nowMs - lastDisconnectRequestAtMs_ < kRetryMs) {
      return false;
    }
    lastDisconnectRequestAtMs_ = nowMs;
    return true;
  }

 private:
  bool active_ = false;
  bool ready_ = false;
  uint32_t connectedAtMs_ = 0;
  uint32_t lastDisconnectRequestAtMs_ = 0;
  uint32_t disconnectStartedAtMs_ = 0;
  bool disconnectPending_ = false;
  bool hardFailed_ = false;
};

}  // namespace pokepod
