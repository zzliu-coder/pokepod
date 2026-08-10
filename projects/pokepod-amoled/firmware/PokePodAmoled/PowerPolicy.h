#pragma once

#include <stdint.h>

namespace pokepod {

enum class PowerMode : uint8_t {
  performance,
  balanced,
  screenOffIdle,
  lightSleep,
};

inline const char *powerModeName(PowerMode mode) {
  switch (mode) {
    case PowerMode::performance: return "performance";
    case PowerMode::balanced: return "balanced";
    case PowerMode::screenOffIdle: return "screen_off_idle";
    case PowerMode::lightSleep: return "light_sleep";
  }
  return "unknown";
}

struct PowerInputs {
  bool screenOn = true;
  bool audioActive = false;
  bool bleConnected = false;
  bool bleStreaming = false;
  bool wifiRadioOn = false;
  bool usbHostConnected = false;
  bool vbusPresent = false;
  bool linkBusy = false;
  bool storageBusy = false;
  bool networkBusy = false;
  bool provisioning = false;
  bool uiAnimating = false;
};

struct PowerDecision {
  PowerMode mode = PowerMode::balanced;
  uint16_t cpuMhz = 80;
  uint16_t touchPollMs = 10;
  uint16_t sensorPollMs = 500;
  bool allowLightSleep = false;
};

inline PowerDecision decidePower(const PowerInputs &input) {
  const bool foreground = input.audioActive || input.bleStreaming ||
      input.wifiRadioOn || input.linkBusy || input.storageBusy ||
      input.networkBusy || input.provisioning || input.uiAnimating;
  if (foreground) {
    return {PowerMode::performance, 240, 10, 500, false};
  }
  if (input.screenOn) {
    return {PowerMode::balanced, 80, 10, 500, false};
  }
  const bool sleepSafe = !input.bleConnected && !input.usbHostConnected &&
      !input.vbusPresent;
  if (sleepSafe) {
    return {PowerMode::lightSleep, 80, 100, 100, true};
  }
  return {PowerMode::screenOffIdle, 80, 40, 250, false};
}

class AutoScreenOffPolicy {
 public:
  static constexpr uint32_t kDefaultTimeoutMs = 30000;

  void begin(uint32_t nowMs) { lastActivityMs_ = nowMs; }
  void noteActivity(uint32_t nowMs) { lastActivityMs_ = nowMs; }
  bool shouldTurnOff(uint32_t nowMs, bool screenOn, bool keepAwake,
                     uint32_t timeoutMs = kDefaultTimeoutMs) const {
    return screenOn && !keepAwake &&
        elapsedSinceActivity(nowMs) >= timeoutMs;
  }
  uint32_t idleMs(uint32_t nowMs) const {
    return elapsedSinceActivity(nowMs);
  }

 private:
  uint32_t elapsedSinceActivity(uint32_t nowMs) const {
    // Touch handling can sample millis() a few milliseconds after the main
    // loop captured nowMs. Treat that small future timestamp as zero idle
    // time instead of letting unsigned subtraction look like 49 days. The
    // signed delta still preserves the normal millis() wrap-around case.
    const int32_t elapsed = static_cast<int32_t>(nowMs - lastActivityMs_);
    return elapsed > 0 ? static_cast<uint32_t>(elapsed) : 0;
  }

  uint32_t lastActivityMs_ = 0;
};

}  // namespace pokepod
