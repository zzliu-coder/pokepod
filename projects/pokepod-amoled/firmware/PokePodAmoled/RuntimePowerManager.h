#pragma once

#include <Arduino.h>

#include "PowerPolicy.h"

namespace pokepod {

struct RuntimePowerSnapshot {
  PowerMode mode = PowerMode::balanced;
  uint16_t cpuMhz = 240;
  uint32_t transitions = 0;
  uint32_t lightSleepCount = 0;
  uint64_t lightSleepUs = 0;
  uint8_t lastWakeCause = 0;
  int32_t lastError = 0;
  bool automaticPmSupported = false;
  bool bleModemSleepSupported = false;
};

class RuntimePowerManager {
 public:
  bool begin(Print &log);
  PowerDecision apply(const PowerInputs &inputs, Print &log);
  bool enterLightSleep(const PowerInputs &verifiedInputs, Print &log);
  const RuntimePowerSnapshot &snapshot() const { return snapshot_; }

 private:
  bool setCpuMhz(uint16_t mhz, Print &log);

  RuntimePowerSnapshot snapshot_;
  PowerDecision decision_;
};

}  // namespace pokepod
