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

enum class CapsuleStorageArea : uint8_t {
  inbox = 0,
  archive,
  trash,
  custom,
};

enum class CapsuleAudioKind : uint8_t {
  none = 0,
  wav,
  m4a,
  other,
};

// Fixed POD storage avoids hundreds of independent String allocations. The
// locator contains only fields needed to locate, sort and filter a capsule.
// Human-readable details are hydrated into CapsuleLibrary's bounded cache.
struct CapsuleLocator {
  char id[37]{};
  char createdAt[32]{};
  uint64_t directoryHash = 0;
  uint16_t flags = 0;
  uint8_t status = 0;
  uint8_t storageArea = static_cast<uint8_t>(CapsuleStorageArea::inbox);
  uint8_t audioKind = static_cast<uint8_t>(CapsuleAudioKind::none);
};

static_assert(sizeof(CapsuleLocator) <= 160,
              "capsule locator exceeds the per-record PSRAM budget");
static_assert(sizeof(CapsuleLocator) * kCapsuleLocatorCapacity <= 80 * 1024,
              "capsule locator index exceeds the total PSRAM budget");

inline uint64_t capsuleDirectoryHash(const char *value) {
  uint64_t hash = UINT64_C(14695981039346656037);
  if (value == nullptr) return hash;
  while (*value != '\0') {
    hash ^= static_cast<uint8_t>(*value++);
    hash *= UINT64_C(1099511628211);
  }
  return hash;
}

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
