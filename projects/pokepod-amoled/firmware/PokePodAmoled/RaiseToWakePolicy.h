#pragma once

#include <math.h>
#include <stdint.h>

namespace pokepod {

class RaiseToWakePolicy {
 public:
  bool update(uint32_t nowMs, bool enabled, bool screenOn,
              float x, float y, float z) {
    if (!isfinite(x) || !isfinite(y) || !isfinite(z)) return false;
    if (!initialized_) {
      lastX_ = x;
      lastY_ = y;
      lastZ_ = z;
      initialized_ = true;
      return false;
    }
    const float movement = fabsf(x - lastX_) + fabsf(y - lastY_) + fabsf(z - lastZ_);
    lastX_ = x;
    lastY_ = y;
    lastZ_ = z;
    if (!enabled || screenOn || static_cast<int32_t>(nowMs - cooldownUntilMs_) < 0) {
      return false;
    }
    const float magnitudeSquared = x * x + y * y + z * z;
    const bool gravityPlausible = magnitudeSquared >= 0.49f && magnitudeSquared <= 1.82f;
    const bool faceVisible = fabsf(z) >= 0.55f;
    if (movement < 0.42f || !gravityPlausible || !faceVisible) return false;
    cooldownUntilMs_ = nowMs + 3000;
    return true;
  }

 private:
  bool initialized_ = false;
  float lastX_ = 0;
  float lastY_ = 0;
  float lastZ_ = 0;
  uint32_t cooldownUntilMs_ = 0;
};

}  // namespace pokepod
