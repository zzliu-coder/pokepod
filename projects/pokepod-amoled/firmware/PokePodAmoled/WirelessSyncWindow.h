#pragma once

#include <stdint.h>

namespace pokepod {

constexpr uint32_t kWirelessSyncWindowMs = 5UL * 60UL * 1000UL;

enum class WirelessSyncWindowPhase : uint8_t {
  closed,
  waitingForNetwork,
  discoverable,
  connected,
};

struct WirelessSyncWindowInputs {
  bool networkConnected = false;
  bool secureServerReady = false;
  bool clientConnected = false;
};

struct WirelessSyncWindowDecision {
  WirelessSyncWindowPhase phase = WirelessSyncWindowPhase::closed;
  bool wifiDemand = false;
  bool listener = false;
  bool bonjour = false;
  uint32_t remainingMs = 0;
};

class WirelessSyncWindow {
 public:
  bool open(uint32_t nowMs) {
    deadlineMs_ = nowMs + kWirelessSyncWindowMs;
    open_ = true;
    return true;
  }

  void close() {
    open_ = false;
    deadlineMs_ = 0;
  }

  WirelessSyncWindowDecision update(
      uint32_t nowMs, const WirelessSyncWindowInputs &inputs) {
    if (!open_) return WirelessSyncWindowDecision();

    if (deadlineReached(nowMs)) {
      close();
      return WirelessSyncWindowDecision();
    }

    WirelessSyncWindowDecision result;
    result.wifiDemand = true;
    result.remainingMs = static_cast<uint32_t>(deadlineMs_ - nowMs);
    if (!inputs.networkConnected || !inputs.secureServerReady) {
      result.phase = WirelessSyncWindowPhase::waitingForNetwork;
      return result;
    }
    result.listener = true;
    result.bonjour = true;
    result.phase = inputs.clientConnected
        ? WirelessSyncWindowPhase::connected
        : WirelessSyncWindowPhase::discoverable;
    return result;
  }

  bool opened() const { return open_; }
  bool deadlineReached(uint32_t nowMs) const {
    return open_ && static_cast<int32_t>(nowMs - deadlineMs_) >= 0;
  }
  uint32_t deadlineMs() const { return deadlineMs_; }

 private:
  bool open_ = false;
  uint32_t deadlineMs_ = 0;
};

inline const char *wirelessSyncWindowPhaseName(
    WirelessSyncWindowPhase phase) {
  switch (phase) {
    case WirelessSyncWindowPhase::closed: return "closed";
    case WirelessSyncWindowPhase::waitingForNetwork: return "waiting-network";
    case WirelessSyncWindowPhase::discoverable: return "discoverable";
    case WirelessSyncWindowPhase::connected: return "connected";
  }
  return "closed";
}

}  // namespace pokepod
