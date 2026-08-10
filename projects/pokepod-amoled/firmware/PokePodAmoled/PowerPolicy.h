#pragma once

#include <stdint.h>

namespace pokepod {

enum class PowerMode : uint8_t {
  performance,
  balanced,
  screenOffIdle,
  lightSleep,
  deepSleepPending,
  safeShutdownPending,
};

inline const char *powerModeName(PowerMode mode) {
  switch (mode) {
    case PowerMode::performance: return "performance";
    case PowerMode::balanced: return "balanced";
    case PowerMode::screenOffIdle: return "screen_off_idle";
    case PowerMode::lightSleep: return "light_sleep";
    case PowerMode::deepSleepPending: return "deep_sleep_pending";
    case PowerMode::safeShutdownPending: return "safe_shutdown_pending";
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
  bool automaticWakeEnabled = true;
  bool criticalBattery = false;
  uint32_t idleMs = 0;
};

struct PowerDecision {
  PowerMode mode = PowerMode::balanced;
  uint16_t cpuMhz = 80;
  uint16_t touchPollMs = 10;
  uint16_t sensorPollMs = 500;
  bool allowLightSleep = false;
  bool requestIdleRadioPause = false;
  bool requestDeepSleep = false;
  bool requestSafeShutdown = false;
  uint64_t lightSleepTimerUs = 0;
};

constexpr uint32_t kLightSleepTimeoutMs = 60000;
constexpr uint32_t kDeepSleepTimeoutMs = 180000;
constexpr uint32_t kChargingLightSleepCheckMs = 30000;

inline bool powerForegroundBusy(const PowerInputs &input) {
  return input.audioActive || input.bleStreaming || input.linkBusy ||
      input.storageBusy || input.networkBusy || input.provisioning ||
      input.uiAnimating;
}

inline PowerDecision decidePower(const PowerInputs &input) {
  const bool foreground = powerForegroundBusy(input);
  if (foreground) {
    return {PowerMode::performance, 240, 10, 500};
  }
  if (input.criticalBattery && !input.vbusPresent) {
    PowerDecision result{PowerMode::safeShutdownPending, 80, 40, 250};
    result.requestSafeShutdown = true;
    return result;
  }
  if (input.screenOn) {
    return {PowerMode::balanced, 80, 10, 500};
  }
  if (input.idleMs >= kDeepSleepTimeoutMs && !input.vbusPresent &&
      !input.usbHostConnected) {
    PowerDecision result{PowerMode::deepSleepPending, 80, 100, 500};
    result.requestIdleRadioPause = true;
    result.requestDeepSleep = true;
    return result;
  }
  if (input.idleMs >= kLightSleepTimeoutMs) {
    PowerDecision result{PowerMode::screenOffIdle, 80, 100, 500};
    result.requestIdleRadioPause = true;
    const bool sleepSafe = !input.bleConnected && !input.wifiRadioOn &&
        !input.usbHostConnected && !input.automaticWakeEnabled;
    if (sleepSafe) {
      result.mode = PowerMode::lightSleep;
      result.allowLightSleep = true;
      const uint32_t remainingMs = input.vbusPresent
          ? kChargingLightSleepCheckMs
          : (kDeepSleepTimeoutMs - input.idleMs);
      result.lightSleepTimerUs = static_cast<uint64_t>(remainingMs) * 1000ULL;
    }
    return result;
  }
  return {PowerMode::screenOffIdle, 80, 40, 250};
}

class LowBatteryShutdownPolicy {
 public:
  static constexpr int kCriticalPercent = 5;
  static constexpr int kRecoveredPercent = 7;
  static constexpr uint8_t kRequiredSamples = 3;

  bool update(int batteryPercent, bool vbusPresent) {
    if (vbusPresent || batteryPercent < 0 ||
        batteryPercent >= kRecoveredPercent) {
      criticalSamples_ = 0;
      critical_ = false;
      return false;
    }
    if (batteryPercent <= kCriticalPercent) {
      if (criticalSamples_ < kRequiredSamples) ++criticalSamples_;
      critical_ = criticalSamples_ >= kRequiredSamples;
    }
    return critical_;
  }

  bool critical() const { return critical_; }
  uint8_t samples() const { return criticalSamples_; }

 private:
  uint8_t criticalSamples_ = 0;
  bool critical_ = false;
};

class AutoScreenOffPolicy {
 public:
  static constexpr uint32_t kDefaultDimTimeoutMs = 12000;
  static constexpr uint32_t kDefaultTimeoutMs = 30000;

  void begin(uint32_t nowMs) { lastActivityMs_ = nowMs; }
  void noteActivity(uint32_t nowMs) { lastActivityMs_ = nowMs; }
  bool shouldTurnOff(uint32_t nowMs, bool screenOn, bool keepAwake,
                     uint32_t timeoutMs = kDefaultTimeoutMs) const {
    return screenOn && !keepAwake &&
        elapsedSinceActivity(nowMs) >= timeoutMs;
  }
  bool shouldDim(uint32_t nowMs, bool screenOn, bool keepAwake,
                 uint32_t timeoutMs = kDefaultDimTimeoutMs) const {
    return screenOn && !keepAwake && elapsedSinceActivity(nowMs) >= timeoutMs;
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
