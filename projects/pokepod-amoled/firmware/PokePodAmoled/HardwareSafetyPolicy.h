#pragma once

#include <stdint.h>
#include <time.h>

namespace pokepod {

constexpr time_t kMinimumTrustedUtcEpoch = 1704067200;

inline bool canPollMotionWake(bool imuAvailable, bool imuLowPower,
                              bool ioExpanderAvailable) {
  return imuAvailable && imuLowPower && ioExpanderAvailable;
}

inline bool systemClockUpdateSucceeded(time_t epoch, int setTimeResult) {
  return epoch >= kMinimumTrustedUtcEpoch && setTimeResult == 0;
}

enum class ShutdownFallbackAction : uint8_t {
  waitForPmuPowerRemoval,
  deepSleepWithBootWake,
  deepSleepWithoutWake,
};

// Reaching this decision means a PMU shutdown request returned to firmware or
// no PMU was available.  A released BOOT line is safe to arm as active-low;
// a held line would wake immediately, so the bounded fallback enters deep
// sleep without a wake source and remains recoverable by reset/USB power.
inline ShutdownFallbackAction shutdownFallbackAction(bool pmuAvailable,
                                                       bool pmuCallReturned,
                                                       bool bootLineReleased) {
  if (pmuAvailable && !pmuCallReturned) {
    return ShutdownFallbackAction::waitForPmuPowerRemoval;
  }
  return bootLineReleased
      ? ShutdownFallbackAction::deepSleepWithBootWake
      : ShutdownFallbackAction::deepSleepWithoutWake;
}

}  // namespace pokepod
