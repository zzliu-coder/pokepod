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

using PowerBlockerMask = uint32_t;

enum class PowerBlocker : uint8_t {
  screenOn = 0,
  audioActive = 1,
  bleRadio = 2,
  bleStreaming = 3,
  wifiRadio = 4,
  usbHost = 5,
  vbusPresent = 6,
  linkBusy = 7,
  storageBusy = 8,
  networkBusy = 9,
  provisioning = 10,
  uiAnimating = 11,
  raiseToWake = 12,
  criticalBattery = 13,
  beforeLightTimeout = 14,
  beforeDeepTimeout = 15,
};

constexpr PowerBlockerMask powerBlockerBit(PowerBlocker blocker) {
  return 1UL << static_cast<uint8_t>(blocker);
}

inline const char *powerBlockerKey(PowerBlocker blocker) {
  switch (blocker) {
    case PowerBlocker::screenOn: return "screen_on";
    case PowerBlocker::audioActive: return "audio_active";
    case PowerBlocker::bleRadio: return "ble_radio";
    case PowerBlocker::bleStreaming: return "ble_streaming";
    case PowerBlocker::wifiRadio: return "wifi_radio";
    case PowerBlocker::usbHost: return "usb_host";
    case PowerBlocker::vbusPresent: return "vbus_present";
    case PowerBlocker::linkBusy: return "link_busy";
    case PowerBlocker::storageBusy: return "storage_busy";
    case PowerBlocker::networkBusy: return "network_busy";
    case PowerBlocker::provisioning: return "provisioning";
    case PowerBlocker::uiAnimating: return "ui_animating";
    case PowerBlocker::raiseToWake: return "raise_to_wake";
    case PowerBlocker::criticalBattery: return "critical_battery";
    case PowerBlocker::beforeLightTimeout: return "before_light_timeout";
    case PowerBlocker::beforeDeepTimeout: return "before_deep_timeout";
  }
  return "unknown";
}

inline PowerBlockerMask activePowerFacts(const PowerInputs &input) {
  PowerBlockerMask mask = 0;
  if (input.screenOn) mask |= powerBlockerBit(PowerBlocker::screenOn);
  if (input.audioActive) mask |= powerBlockerBit(PowerBlocker::audioActive);
  if (input.bleConnected) mask |= powerBlockerBit(PowerBlocker::bleRadio);
  if (input.bleStreaming) mask |= powerBlockerBit(PowerBlocker::bleStreaming);
  if (input.wifiRadioOn) mask |= powerBlockerBit(PowerBlocker::wifiRadio);
  if (input.usbHostConnected) mask |= powerBlockerBit(PowerBlocker::usbHost);
  if (input.vbusPresent) mask |= powerBlockerBit(PowerBlocker::vbusPresent);
  if (input.linkBusy) mask |= powerBlockerBit(PowerBlocker::linkBusy);
  if (input.storageBusy) mask |= powerBlockerBit(PowerBlocker::storageBusy);
  if (input.networkBusy) mask |= powerBlockerBit(PowerBlocker::networkBusy);
  if (input.provisioning) mask |= powerBlockerBit(PowerBlocker::provisioning);
  if (input.uiAnimating) mask |= powerBlockerBit(PowerBlocker::uiAnimating);
  if (input.automaticWakeEnabled) {
    mask |= powerBlockerBit(PowerBlocker::raiseToWake);
  }
  if (input.criticalBattery) {
    mask |= powerBlockerBit(PowerBlocker::criticalBattery);
  }
  return mask;
}

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

inline PowerBlockerMask lightSleepBlockers(const PowerInputs &input) {
  PowerBlockerMask mask = 0;
  if (input.idleMs < kLightSleepTimeoutMs) {
    mask |= powerBlockerBit(PowerBlocker::beforeLightTimeout);
  }
  const PowerBlockerMask facts = activePowerFacts(input);
  const PowerBlockerMask relevant =
      powerBlockerBit(PowerBlocker::screenOn) |
      powerBlockerBit(PowerBlocker::audioActive) |
      powerBlockerBit(PowerBlocker::bleRadio) |
      powerBlockerBit(PowerBlocker::bleStreaming) |
      powerBlockerBit(PowerBlocker::wifiRadio) |
      powerBlockerBit(PowerBlocker::usbHost) |
      powerBlockerBit(PowerBlocker::linkBusy) |
      powerBlockerBit(PowerBlocker::storageBusy) |
      powerBlockerBit(PowerBlocker::networkBusy) |
      powerBlockerBit(PowerBlocker::provisioning) |
      powerBlockerBit(PowerBlocker::uiAnimating) |
      powerBlockerBit(PowerBlocker::raiseToWake) |
      powerBlockerBit(PowerBlocker::criticalBattery);
  return mask | (facts & relevant);
}

inline PowerBlockerMask deepSleepBlockers(const PowerInputs &input) {
  PowerBlockerMask mask = 0;
  if (input.idleMs < kDeepSleepTimeoutMs) {
    mask |= powerBlockerBit(PowerBlocker::beforeDeepTimeout);
  }
  const PowerBlockerMask facts = activePowerFacts(input);
  const PowerBlockerMask relevant =
      powerBlockerBit(PowerBlocker::screenOn) |
      powerBlockerBit(PowerBlocker::audioActive) |
      powerBlockerBit(PowerBlocker::bleRadio) |
      powerBlockerBit(PowerBlocker::bleStreaming) |
      powerBlockerBit(PowerBlocker::wifiRadio) |
      powerBlockerBit(PowerBlocker::usbHost) |
      powerBlockerBit(PowerBlocker::vbusPresent) |
      powerBlockerBit(PowerBlocker::linkBusy) |
      powerBlockerBit(PowerBlocker::storageBusy) |
      powerBlockerBit(PowerBlocker::networkBusy) |
      powerBlockerBit(PowerBlocker::provisioning) |
      powerBlockerBit(PowerBlocker::uiAnimating) |
      powerBlockerBit(PowerBlocker::criticalBattery);
  return mask | (facts & relevant);
}

inline PowerBlockerMask currentSleepBlockers(const PowerInputs &input) {
  return input.idleMs >= kDeepSleepTimeoutMs
      ? deepSleepBlockers(input) : lightSleepBlockers(input);
}

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
