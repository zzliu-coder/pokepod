#pragma once

#include <stdint.h>

#include "PowerFacts.h"

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
  bool usbMounted = false;
  bool cdcSessionActive = false;
  bool vbusPresent = false;
  bool charging = false;
  bool linkBusy = false;
  bool linkLeaseActive = false;
  bool storageBusy = false;
  bool storageMutationActive = false;
  bool storageReadActive = false;
  bool networkBusy = false;
  bool provisioning = false;
  bool uiAnimating = false;
  bool automaticWakeEnabled = true;
  bool wakeSourcesReady = false;
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
  charging = 16,
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
    case PowerBlocker::charging: return "charging";
  }
  return "unknown";
}

inline bool inputBleRadioActive(const PowerInputs &input) {
  return input.bleConnected;
}

inline bool inputCdcSessionActive(const PowerInputs &input) {
  // usbHostConnected remains as a compatibility alias until PokePodApp's
  // single-owner integration switches to cdcSessionActive.
  return input.cdcSessionActive || input.usbHostConnected;
}

inline bool inputLinkLeaseActive(const PowerInputs &input) {
  return input.linkLeaseActive || input.linkBusy;
}

inline bool inputStorageMutationActive(const PowerInputs &input) {
  return input.storageMutationActive || input.storageReadActive ||
      input.storageBusy;
}

inline bool automaticWakeUnavailable(const PowerInputs &input) {
  return input.automaticWakeEnabled && !input.wakeSourcesReady;
}

inline PowerInputs powerInputsWithFacts(PowerInputs input,
                                        const PowerFacts &facts) {
  input.usbMounted = facts.usbMounted;
  input.cdcSessionActive = facts.cdcSessionActive;
  input.usbHostConnected = false;
  input.vbusPresent = facts.vbusPresent;
  input.charging = facts.charging;
  input.linkLeaseActive = facts.linkLeaseActive;
  input.linkBusy = false;
  input.bleConnected = facts.bleRadioActive;
  input.bleStreaming = facts.bleStreaming;
  input.wifiRadioOn = facts.wifiRadioActive;
  input.wakeSourcesReady = facts.wakeSourcesReady;
  input.storageMutationActive = facts.storageMutationActive;
  input.storageReadActive = facts.storageReadActive;
  input.storageBusy = false;
  return input;
}

inline PowerBlockerMask activePowerFacts(const PowerInputs &input) {
  PowerBlockerMask mask = 0;
  if (input.screenOn) mask |= powerBlockerBit(PowerBlocker::screenOn);
  if (input.audioActive) mask |= powerBlockerBit(PowerBlocker::audioActive);
  if (inputBleRadioActive(input)) {
    mask |= powerBlockerBit(PowerBlocker::bleRadio);
  }
  if (input.bleStreaming) mask |= powerBlockerBit(PowerBlocker::bleStreaming);
  if (input.wifiRadioOn) mask |= powerBlockerBit(PowerBlocker::wifiRadio);
  if (inputCdcSessionActive(input)) {
    mask |= powerBlockerBit(PowerBlocker::usbHost);
  }
  if (input.vbusPresent) mask |= powerBlockerBit(PowerBlocker::vbusPresent);
  if (input.charging) mask |= powerBlockerBit(PowerBlocker::charging);
  if (inputLinkLeaseActive(input)) {
    mask |= powerBlockerBit(PowerBlocker::linkBusy);
  }
  if (inputStorageMutationActive(input)) {
    mask |= powerBlockerBit(PowerBlocker::storageBusy);
  }
  if (input.networkBusy) mask |= powerBlockerBit(PowerBlocker::networkBusy);
  if (input.provisioning) mask |= powerBlockerBit(PowerBlocker::provisioning);
  if (input.uiAnimating) mask |= powerBlockerBit(PowerBlocker::uiAnimating);
  if (automaticWakeUnavailable(input)) {
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
      powerBlockerBit(PowerBlocker::charging) |
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
  return input.audioActive || input.bleStreaming ||
      inputLinkLeaseActive(input) || inputStorageMutationActive(input) ||
      input.networkBusy || input.provisioning || input.uiAnimating;
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
  if (input.idleMs >= kDeepSleepTimeoutMs) {
    PowerDecision result{PowerMode::screenOffIdle, 80, 100, 500};
    result.requestIdleRadioPause = true;
    const PowerBlockerMask deepBlockers = deepSleepBlockers(input);
    const PowerBlockerMask radioBlockers =
        powerBlockerBit(PowerBlocker::bleRadio) |
        powerBlockerBit(PowerBlocker::wifiRadio);
    const bool directPowerFactsPermitDeepSleep =
        !input.vbusPresent && !input.charging;
    if ((deepBlockers & ~radioBlockers) == 0 &&
        directPowerFactsPermitDeepSleep) {
      result.mode = PowerMode::deepSleepPending;
      result.requestDeepSleep = true;
    } else if (lightSleepBlockers(input) == 0) {
      // Charging/VBUS intentionally prevents deep sleep. Keep checking in
      // bounded light-sleep slices instead of remaining fully awake forever.
      result.mode = PowerMode::lightSleep;
      result.allowLightSleep = true;
      result.lightSleepTimerUs =
          static_cast<uint64_t>(kChargingLightSleepCheckMs) * 1000ULL;
    }
    return result;
  }
  if (input.idleMs >= kLightSleepTimeoutMs) {
    PowerDecision result{PowerMode::screenOffIdle, 80, 100, 500};
    result.requestIdleRadioPause = true;
    const bool directSessionFactsPermitLightSleep =
        !input.bleConnected && !input.wifiRadioOn &&
        !input.usbHostConnected &&
        (!input.automaticWakeEnabled || input.wakeSourcesReady);
    if (lightSleepBlockers(input) == 0 &&
        directSessionFactsPermitLightSleep) {
      result.mode = PowerMode::lightSleep;
      result.allowLightSleep = true;
      const uint32_t remainingMs = (input.vbusPresent || input.charging)
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

enum class SafeShutdownProgress : uint8_t {
  idle,
  waitingForServices,
  ready,
};

class SafeShutdownQuiescePolicy {
 public:
  void request() { pending_ = true; }
  bool pending() const { return pending_; }

  SafeShutdownProgress update(bool servicesQuiesced) {
    if (!pending_) return SafeShutdownProgress::idle;
    if (!servicesQuiesced) return SafeShutdownProgress::waitingForServices;
    pending_ = false;
    return SafeShutdownProgress::ready;
  }

 private:
  bool pending_ = false;
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
