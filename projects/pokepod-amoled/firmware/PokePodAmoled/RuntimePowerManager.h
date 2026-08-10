#pragma once

#include <Arduino.h>

#include "PowerPolicy.h"

namespace pokepod {

constexpr int32_t kPowerErrorBootWakeLineHeld = -7001;

struct RuntimePowerSnapshot {
  PowerMode mode = PowerMode::balanced;
  uint16_t cpuMhz = 240;
  uint32_t transitions = 0;
  uint32_t lightSleepCount = 0;
  uint32_t lightSleepAttempts = 0;
  uint32_t lightSleepFailures = 0;
  uint64_t lightSleepUs = 0;
  uint32_t lastLightSleepMs = 0;
  uint32_t deepSleepArmAttempts = 0;
  uint32_t deepSleepArmFailures = 0;
  uint32_t deepSleepWakeCount = 0;
  uint8_t lastWakeCause = 0;
  uint32_t wakeCauses = 0;
  uint64_t ext1WakeMask = 0;
  uint64_t deepSleepWakeMask = 0;
  int32_t lastError = 0;
  bool automaticPmSupported = false;
  bool bleModemSleepSupported = false;
  bool wokeFromDeepSleep = false;
  bool deepSleepBootWakeArmed = false;
  bool deepSleepTouchWakeArmed = false;
};

class RuntimePowerManager {
 public:
  bool begin(Print &log);
  PowerDecision apply(const PowerInputs &inputs, Print &log);
  bool enterLightSleep(const PowerInputs &verifiedInputs,
                       const PowerDecision &verifiedDecision, Print &log);
  bool armDeepSleepWakeSources(bool touchWakeEnabled, Print &log);
  [[noreturn]] void startDeepSleep(Print &log);
  const RuntimePowerSnapshot &snapshot() const { return snapshot_; }

 private:
  bool setCpuMhz(uint16_t mhz, Print &log);

  RuntimePowerSnapshot snapshot_;
  PowerDecision decision_;
};

}  // namespace pokepod
