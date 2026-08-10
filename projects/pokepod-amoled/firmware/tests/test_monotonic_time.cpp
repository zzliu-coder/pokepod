#include <cassert>
#include <cstdint>

#include "MonotonicTime.h"

using namespace pokepod;

int main() {
  assert(monotonicElapsedSigned(1200, 1000) == 200);
  assert(monotonicElapsedAtLeast(1200, 1000, 200));
  assert(!monotonicElapsedAtLeast(1199, 1000, 200));

  // Reproduce the real provisioning failure: the loop clock was captured six
  // milliseconds before the HTTP handler stored validatingSinceMs_.
  assert(monotonicElapsedSigned(1000, 1006) == -6);
  assert(monotonicElapsedOrZero(1000, 1006) == 0);
  assert(!monotonicElapsedAtLeast(1000, 1006, 15000));

  // uint32_t millis() wrap must continue to work.
  constexpr uint32_t beforeWrap = 0xfffffff0U;
  assert(monotonicElapsedOrZero(0x00000004U, beforeWrap) == 20);
  assert(monotonicElapsedAtLeast(0x00000004U, beforeWrap, 20));
  assert(!monotonicElapsedAtLeast(0x00000004U, beforeWrap, 21));
  return 0;
}
