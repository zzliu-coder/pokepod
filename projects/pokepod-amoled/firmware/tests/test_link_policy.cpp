#include <cassert>

#include "../PokePodAmoled/LinkPolicy.h"

int main() {
  using namespace pokepod;
  assert(safeLinkRelativePath("Inbox/a/capsule.json"));
  assert(safeLinkRelativePath(".trash/a/trash.json", true));
  assert(!safeLinkRelativePath(""));
  assert(!safeLinkRelativePath("/Inbox/a"));
  assert(!safeLinkRelativePath("Inbox/../.system/secret"));
  assert(!safeLinkRelativePath("Inbox//a"));
  assert(!safeLinkRelativePath("Inbox\\a"));
  assert(!safeLinkRelativePath(".staging/a", false));

  assert(purgeOutcome(false, false) == PurgeOutcome::rejected);
  assert(purgeOutcome(true, true) == PurgeOutcome::committed);
  assert(purgeOutcome(true, false) ==
         PurgeOutcome::committedCleanupDeferred);
  assert(purgeStagingDirectoryName("purge-1234"));
  assert(!purgeStagingDirectoryName("purge-"));
  assert(!purgeStagingDirectoryName("recording-1234"));

  LinkRequestHistory history;
  assert(!history.contains(7));
  assert(history.complete(7));
  assert(history.contains(7));
  assert(!history.complete(7));
  for (uint32_t id = 8; id < 40; ++id) assert(history.complete(id));
  assert(!history.contains(7));
  assert(history.contains(39));
  history.clear();
  assert(!history.contains(39));
  assert(history.contains(0));
  return 0;
}
