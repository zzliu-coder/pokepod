#pragma once

#include <cstdint>

namespace pokepod {

class BleAppHandshakePolicy {
 public:
  static constexpr uint32_t kTimeoutMs = 15000;
  static constexpr uint32_t kRetryMs = 250;

  void connected(uint32_t nowMs) {
    active_ = true;
    ready_ = false;
    connectedAtMs_ = nowMs;
    lastDisconnectRequestAtMs_ = 0;
  }

  void ready() { ready_ = true; }

  void disconnected() {
    active_ = false;
    ready_ = false;
    connectedAtMs_ = 0;
    lastDisconnectRequestAtMs_ = 0;
  }

  bool requestDisconnect(uint32_t nowMs, bool pairingActive) {
    if (!active_ || ready_ || pairingActive ||
        nowMs - connectedAtMs_ < kTimeoutMs) {
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
};

}  // namespace pokepod
