#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "CapsuleBrowserState.h"

namespace pokepod {

constexpr size_t kCapsuleLocatorCapacity = 512;
constexpr size_t kCapsuleDetailCacheCapacity = 12;

enum CapsuleLocatorFlag : uint16_t {
  locatorFavorite = 1U << 0,
  locatorArchived = 1U << 1,
  locatorTrashed = 1U << 2,
  locatorReadOnly = 1U << 3,
  locatorPending = 1U << 4,
  locatorFailed = 1U << 5,
  locatorDamaged = 1U << 6,
};

// Fixed POD storage avoids hundreds of independent String allocations.  It is
// allocated in PSRAM by CapsuleLibrary and rebuilt from the capsule folders.
struct CapsuleLocator {
  char id[37]{};
  char directory[384]{};
  char folder[256]{};
  char title[128]{};
  char createdAt[32]{};
  char updatedAt[32]{};
  char audioFile[48]{};
  char audioFormat[48]{};
  char errorStage[64]{};
  char error[192]{};
  int32_t capsuleSchemaVersion = -1;
  int32_t processingSchemaVersion = -1;
  int32_t revision = -1;
  int32_t processingRevision = -1;
  uint32_t durationMs = 0;
  uint32_t sampleRateHz = 0;
  uint16_t flags = 0;
  uint8_t channels = 0;
  uint8_t bitsPerSample = 0;
  uint8_t status = 0;
};

constexpr bool capsuleLocatorHasFlag(const CapsuleLocator &locator,
                                     CapsuleLocatorFlag flag) {
  return (locator.flags & static_cast<uint16_t>(flag)) != 0;
}

constexpr bool capsuleLocatorVisible(const CapsuleLocator &locator,
                                     CapsuleScope scope) {
  return capsuleVisibleInScope(
      scope,
      capsuleLocatorHasFlag(locator, locatorFavorite),
      capsuleLocatorHasFlag(locator, locatorPending),
      capsuleLocatorHasFlag(locator, locatorFailed),
      capsuleLocatorHasFlag(locator, locatorArchived),
      capsuleLocatorHasFlag(locator, locatorTrashed));
}

inline bool capsuleLocatorNewer(const CapsuleLocator &left,
                                const CapsuleLocator &right) {
  const int timeOrder = strcmp(left.createdAt, right.createdAt);
  return timeOrder == 0 ? strcmp(left.id, right.id) > 0 : timeOrder > 0;
}

}  // namespace pokepod
