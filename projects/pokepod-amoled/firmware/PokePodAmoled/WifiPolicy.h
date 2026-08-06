#pragma once

#include <stdint.h>

namespace pokepod {

constexpr uint32_t kWifiGraceMs = 3UL * 60UL * 1000UL;

enum class WifiPhase {
  disabled,
  off,
  connecting,
  online,
  grace,
  provisioning,
  error,
};

struct WifiInputs {
  bool configured = false;
  bool manuallyDisabled = false;
  bool charging = false;
  bool recording = false;
  bool pendingWork = false;
  bool connected = false;
  bool provisioning = false;
  bool connectionFailed = false;
};

struct WifiDecision {
  WifiPhase phase = WifiPhase::disabled;
  bool radioOn = false;
  bool processQueue = false;
  uint32_t idleSinceMs = 0;
};

inline WifiDecision nextWifiDecision(const WifiDecision &previous,
                                     const WifiInputs &inputs,
                                     uint32_t nowMs) {
  WifiDecision result = previous;
  if (inputs.provisioning) {
    result.phase = WifiPhase::provisioning;
    result.radioOn = true;
    result.processQueue = false;
    result.idleSinceMs = 0;
    return result;
  }
  if (!inputs.configured || inputs.manuallyDisabled) {
    result.phase = inputs.configured ? WifiPhase::off : WifiPhase::disabled;
    result.radioOn = false;
    result.processQueue = false;
    result.idleSinceMs = 0;
    return result;
  }

  const bool demand = inputs.charging || inputs.recording || inputs.pendingWork;
  if (demand) {
    result.phase = inputs.connected ? WifiPhase::online :
        (inputs.connectionFailed ? WifiPhase::error : WifiPhase::connecting);
    result.radioOn = true;
    result.processQueue = inputs.connected && inputs.pendingWork;
    result.idleSinceMs = 0;
    return result;
  }

  if (!previous.radioOn) {
    result.phase = WifiPhase::off;
    result.radioOn = false;
    result.processQueue = false;
    result.idleSinceMs = 0;
    return result;
  }
  if (result.idleSinceMs == 0) result.idleSinceMs = nowMs;
  if (static_cast<uint32_t>(nowMs - result.idleSinceMs) >= kWifiGraceMs) {
    result.phase = WifiPhase::off;
    result.radioOn = false;
    result.idleSinceMs = 0;
  } else {
    result.phase = WifiPhase::grace;
    result.radioOn = true;
  }
  result.processQueue = false;
  return result;
}

inline uint32_t wifiRetryDelayMs(uint8_t failedAttempt) {
  switch (failedAttempt) {
    case 0: return 10000;
    case 1: return 30000;
    case 2: return 120000;
    default: return 0;
  }
}

}  // namespace pokepod
