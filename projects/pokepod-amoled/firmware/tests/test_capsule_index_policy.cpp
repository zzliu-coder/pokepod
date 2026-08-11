#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

#include "CapsuleIndexPolicy.h"

using namespace pokepod;

namespace {

CapsuleLocator makeLocator(size_t ordinal) {
  CapsuleLocator locator;
  std::snprintf(locator.id, sizeof(locator.id),
                "00000000-0000-4000-8000-%012zu", ordinal);
  std::snprintf(locator.createdAt, sizeof(locator.createdAt),
                "2026-08-%02zuT%02zu:%02zu:%02zuZ",
                (ordinal / 86400) % 28 + 1,
                (ordinal / 3600) % 24,
                (ordinal / 60) % 60,
                ordinal % 60);
  if (ordinal % 7 == 0) locator.flags |= locatorFavorite;
  if (ordinal % 11 == 0) locator.flags |= locatorPending;
  if (ordinal % 13 == 0) locator.flags |= locatorFailed;
  if (ordinal % 17 == 0) locator.flags |= locatorArchived;
  if (ordinal % 19 == 0) locator.flags |= locatorTrashed;
  return locator;
}

}  // namespace

int main() {
  static_assert(kCapsuleLocatorCapacity >= 320);
  static_assert(sizeof(CapsuleLocator) < 1280);

  std::vector<CapsuleLocator> locators;
  locators.reserve(320);
  // Directory enumeration order is intentionally opposite to timestamp order.
  for (size_t ordinal = 320; ordinal > 0; --ordinal) {
    locators.push_back(makeLocator(ordinal - 1));
  }
  assert(locators.size() == 320);
  std::sort(locators.begin(), locators.end(), capsuleLocatorNewer);
  assert(std::strcmp(locators.front().id,
                     "00000000-0000-4000-8000-000000000319") == 0);
  const auto newestPending = std::find_if(
      locators.begin(), locators.end(), [](const CapsuleLocator &locator) {
        return capsuleLocatorHasFlag(locator, locatorPending);
      });
  assert(newestPending != locators.end());
  assert(std::strcmp(newestPending->id,
                     "00000000-0000-4000-8000-000000000319") == 0);

  size_t inbox = 0;
  size_t pending = 0;
  size_t failed = 0;
  size_t archived = 0;
  size_t trashed = 0;
  for (const CapsuleLocator &locator : locators) {
    inbox += capsuleLocatorVisible(locator, CapsuleScope::inbox);
    pending += capsuleLocatorVisible(locator, CapsuleScope::pending);
    failed += capsuleLocatorVisible(locator, CapsuleScope::failed);
    archived += capsuleLocatorVisible(locator, CapsuleScope::archive);
    trashed += capsuleLocatorVisible(locator, CapsuleScope::trash);
  }
  assert(inbox > 96);
  assert(pending > 0 && failed > 0 && archived > 0 && trashed > 0);

  CapsuleLocator damaged = makeLocator(400);
  damaged.flags |= locatorDamaged | locatorReadOnly;
  std::strcpy(damaged.audioFile, "audio.wav");
  assert(capsuleLocatorHasFlag(damaged, locatorDamaged));
  assert(capsuleLocatorHasFlag(damaged, locatorReadOnly));
  assert(std::strcmp(damaged.audioFile, "audio.wav") == 0);
  assert(capsuleLocatorVisible(damaged, CapsuleScope::inbox));

  CapsuleLocator future = makeLocator(401);
  future.processingSchemaVersion = 99;
  future.flags |= locatorReadOnly;
  assert(capsuleLocatorHasFlag(future, locatorReadOnly));
  assert(capsuleLocatorVisible(future, CapsuleScope::inbox));
  std::printf("PASS capsule_index_policy (320 fixtures, locator=%zu bytes, "
              "index=%zu bytes)\n",
              sizeof(CapsuleLocator),
              sizeof(CapsuleLocator) * kCapsuleLocatorCapacity);
  return 0;
}
