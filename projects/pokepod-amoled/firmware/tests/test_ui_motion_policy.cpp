#include <assert.h>
#include <stdint.h>

#include "UiMotionPolicy.h"

int main() {
  using pokepod::UiMotionPolicy;

  assert(UiMotionPolicy::smoothPeak(0, 4000) == 1000);
  assert(UiMotionPolicy::smoothPeak(1000, 1000) == 1000);

  const uint16_t samples[] = {0, 200, 820, 400};
  assert(UiMotionPolicy::frameMaximum(samples, 4) == 820);
  assert(UiMotionPolicy::trackCeiling(700, 820) == 820);
  assert(UiMotionPolicy::trackCeiling(700, 0) == 700);
  assert(UiMotionPolicy::trackCeiling(1600, 0) == 1550);

  assert(UiMotionPolicy::breath(0) == 0);
  assert(UiMotionPolicy::breath(12) == 12);
  assert(UiMotionPolicy::breath(18) == 6);
  assert(UiMotionPolicy::breath(24) == 0);

  assert(UiMotionPolicy::energy(0, 700) == 0);
  assert(UiMotionPolicy::energy(700, 700) == 18);
  assert(UiMotionPolicy::energy(1400, 700) == 18);
  assert(UiMotionPolicy::energy(500, 0) == 0);

  assert(UiMotionPolicy::barHeight(0, 700) == 4);
  assert(UiMotionPolicy::barHeight(700, 700) == 56);
  assert(UiMotionPolicy::barHeight(1400, 700) == 58);
  assert(UiMotionPolicy::barHeight(500, 0) == 4);
  return 0;
}
