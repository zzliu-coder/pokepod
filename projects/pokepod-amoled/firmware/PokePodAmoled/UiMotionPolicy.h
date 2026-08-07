#pragma once

#include <stddef.h>
#include <stdint.h>

namespace pokepod {

struct UiMotionPolicy {
  static constexpr uint16_t kMinimumCeiling = 700;
  static constexpr uint8_t kBreathPeriod = 24;
  static constexpr uint8_t kMaximumEnergy = 18;
  static constexpr int16_t kMinimumBarHeight = 4;
  static constexpr int16_t kMaximumBarHeight = 58;

  static uint16_t smoothPeak(uint16_t previous, uint16_t sample) {
    return static_cast<uint16_t>(
        (static_cast<uint32_t>(previous) * 3 + sample) / 4);
  }

  static uint16_t frameMaximum(const uint16_t *samples, size_t count) {
    uint16_t maximum = 0;
    for (size_t index = 0; index < count; ++index) {
      if (samples[index] > maximum) maximum = samples[index];
    }
    return maximum;
  }

  static uint16_t trackCeiling(uint16_t previous, uint16_t maximum) {
    uint16_t next = maximum > previous ? maximum : static_cast<uint16_t>(
        (static_cast<uint32_t>(previous) * 31 + maximum) / 32);
    return next < kMinimumCeiling ? kMinimumCeiling : next;
  }

  static uint8_t breath(uint8_t tick) {
    const uint8_t phase = tick % kBreathPeriod;
    return phase <= kBreathPeriod / 2 ? phase : kBreathPeriod - phase;
  }

  static uint8_t energy(uint16_t smoothedPeak, uint16_t ceiling) {
    if (ceiling == 0) return 0;
    uint32_t scaled = static_cast<uint32_t>(smoothedPeak) *
        kMaximumEnergy / ceiling;
    if (scaled > kMaximumEnergy) scaled = kMaximumEnergy;
    return static_cast<uint8_t>(scaled);
  }

  static int16_t barHeight(uint16_t sample, uint16_t ceiling) {
    if (ceiling == 0) return kMinimumBarHeight;
    int16_t height = kMinimumBarHeight +
        static_cast<int32_t>(sample) * 52 / ceiling;
    if (height > kMaximumBarHeight) height = kMaximumBarHeight;
    return height;
  }
};

}  // namespace pokepod
