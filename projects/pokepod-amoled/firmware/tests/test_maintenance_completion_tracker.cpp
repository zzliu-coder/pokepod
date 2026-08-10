#include <cassert>

#include "../PokePodAmoled/MaintenanceCompletionTracker.h"

using namespace pokepod;

int main() {
  static constexpr const char *kEndId =
      "22222222-2222-4222-8222-222222222222";
  static constexpr const char *kOtherId =
      "33333333-3333-4333-8333-333333333333";

  MaintenanceCompletionTracker tracker;
  tracker.beginAccepted();
  assert(tracker.startRevision() == 1);
  assert(tracker.completionRevision() == 0);

  // fingerprint/read are intentionally read-only and do not affect the
  // maintenance lifecycle.
  assert(!tracker.pendingEnd());
  assert(tracker.startRevision() == 1);
  assert(tracker.completionRevision() == 0);

  tracker.endResultPersisted(kEndId);
  assert(tracker.pendingEnd());
  assert(tracker.completionRevision() == 0);
  assert(!tracker.resultFetched(kOtherId, true));
  assert(!tracker.resultFetched(kEndId, false));
  assert(tracker.pendingEnd());
  assert(tracker.completionRevision() == 0);

  assert(tracker.resultFetched(kEndId, true));
  assert(!tracker.pendingEnd());
  assert(tracker.completionRevision() == 1);
  assert(tracker.completedStartRevision() == tracker.startRevision());

  // Closing the socket after the result was fetched preserves completion.
  tracker.disconnect();
  assert(tracker.completionRevision() == 1);
  assert(!tracker.resultFetched(kEndId, true));

  // A new maintenance transaction invalidates an unread prior end result.
  tracker.endResultPersisted(kEndId);
  tracker.beginAccepted();
  assert(tracker.startRevision() == 2);
  assert(!tracker.pendingEnd());
  assert(!tracker.resultFetched(kEndId, true));
  assert(tracker.completionRevision() == 1);
  // If an observer sees the old completion and this later begin in one
  // snapshot, the generations differ and the old completion must be ignored.
  assert(tracker.completedStartRevision() != tracker.startRevision());

  // A disconnect before result fetch can never create completion later.
  tracker.endResultPersisted(kOtherId);
  tracker.disconnect();
  assert(!tracker.resultFetched(kOtherId, true));
  assert(tracker.completionRevision() == 1);

  // Accepting another transaction invalidates the prior completed
  // presentation immediately. A later failure to persist the begin result,
  // followed by disconnect, cannot create or restore completion.
  const uint32_t completedBeforeFailedBegin = tracker.completionRevision();
  tracker.beginAccepted();
  assert(tracker.startRevision() == 3);
  assert(!tracker.pendingEnd());
  tracker.disconnect();
  assert(tracker.startRevision() == 3);
  assert(tracker.completionRevision() == completedBeforeFailedBegin);

  // A completion fetched for the current generation remains presentable even
  // when an observer first sees both revisions in the same snapshot.
  tracker.endResultPersisted(kOtherId);
  assert(tracker.resultFetched(kOtherId, true));
  assert(tracker.completedStartRevision() == tracker.startRevision());
  assert(tracker.completionRevision() == completedBeforeFailedBegin + 1);
  return 0;
}
