#pragma once

#include <stddef.h>
#include <stdint.h>

namespace pokepod {

// Large services are mandatory PSRAM clients on the production board. A
// failed external allocation is a capability failure, never permission to
// consume the internal heap reserved for display and radio drivers.
enum class PsramServiceState : uint8_t {
  unallocated = 0,
  ready = 1,
  degraded = 2,
};

struct PsramAllocationDecision {
  PsramServiceState state = PsramServiceState::unallocated;
  size_t bytes = 0;
};

constexpr PsramAllocationDecision psramAllocationSucceeded(size_t bytes) {
  return {PsramServiceState::ready, bytes};
}

constexpr PsramAllocationDecision psramAllocationFailed(size_t bytes) {
  return {PsramServiceState::degraded, bytes};
}

constexpr bool psramDegraded(PsramServiceState state) {
  return state == PsramServiceState::degraded;
}

// Explicit contract: large services may never silently fall back to internal
// RAM when production PSRAM allocation fails.
constexpr bool psramMayFallbackToInternal() { return false; }

}  // namespace pokepod
