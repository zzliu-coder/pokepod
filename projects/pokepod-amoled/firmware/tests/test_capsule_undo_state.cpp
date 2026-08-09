#include <assert.h>

#include "CapsuleUndoState.h"

using namespace pokepod;

int main() {
  CapsuleUndoState state;
  state.arm({"a", "b", "b", ""}, 100);
  assert(state.pendingIds().size() == 2);
  assert(state.available(100));
  assert(state.remainingMs(100) == 5000);

  // A partial restore removes only successful IDs and preserves the precise
  // failures for another tap during the original five-second window.
  CapsuleUndoResult partial = state.finishAttempt({"b", "unknown"});
  assert(partial.attempted == 2);
  assert(partial.restored == 1);
  assert(partial.failed() == 1);
  assert(partial.failedIds[0] == "b");
  assert(state.pendingIds().size() == 1);
  assert(state.pendingIds()[0] == "b");
  assert(state.remainingMs(200) == 4900);

  CapsuleUndoResult complete = state.finishAttempt({});
  assert(complete.complete());
  assert(complete.restored == 1);
  assert(!state.available(201));

  state.arm({"c"}, UINT32_MAX - 100);
  assert(state.available(UINT32_MAX - 50));
  assert(state.available(200));
  assert(!state.available(5000));
  assert(state.expire(5000));
  assert(state.pendingIds().empty());
  assert(!state.expire(5001));
  state.clear();
  assert(state.pendingIds().empty());
  return 0;
}
