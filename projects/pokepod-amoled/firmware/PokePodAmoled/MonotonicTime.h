#pragma once

#include <stdint.h>

namespace pokepod {

// A handler may store a timestamp after its caller captured the loop clock.
// Treat that slightly older clock as "not started yet" while preserving the
// normal uint32_t millis() wrap behaviour.
inline int32_t monotonicElapsedSigned(uint32_t nowMs, uint32_t sinceMs) {
  return static_cast<int32_t>(nowMs - sinceMs);
}

inline bool monotonicElapsedAtLeast(uint32_t nowMs, uint32_t sinceMs,
                                    uint32_t durationMs) {
  const int32_t elapsed = monotonicElapsedSigned(nowMs, sinceMs);
  return elapsed >= 0 && static_cast<uint32_t>(elapsed) >= durationMs;
}

inline uint32_t monotonicElapsedOrZero(uint32_t nowMs, uint32_t sinceMs) {
  const int32_t elapsed = monotonicElapsedSigned(nowMs, sinceMs);
  return elapsed < 0 ? 0U : static_cast<uint32_t>(elapsed);
}

}  // namespace pokepod
