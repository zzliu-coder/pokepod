#include <assert.h>
#include <string>
#include <vector>

#include "CapsuleBrowserState.h"

using namespace pokepod;

int main() {
  assert(capsuleVisibleInScope(CapsuleScope::inbox, false, false, false,
                               false, false));
  assert(!capsuleVisibleInScope(CapsuleScope::inbox, false, false, false,
                                true, false));
  assert(capsuleVisibleInScope(CapsuleScope::favorites, true, false, false,
                               true, false));
  assert(capsuleVisibleInScope(CapsuleScope::pending, false, true, false,
                               false, false));
  assert(capsuleVisibleInScope(CapsuleScope::failed, false, false, true,
                               false, false));
  assert(capsuleVisibleInScope(CapsuleScope::archive, false, false, false,
                               true, false));
  assert(capsuleVisibleInScope(CapsuleScope::trash, false, false, false,
                               false, true));
  assert(nextCapsuleScope(CapsuleScope::trash) == CapsuleScope::inbox);

  CapsuleRollbackCounts rollback;
  rollback.record(true);
  rollback.record(false);
  rollback.record(true);
  assert(rollback.attempted == 3);
  assert(rollback.failed == 1);
  assert(!rollback.fullyRolledBack());
  CapsuleRollbackCounts completeRollback;
  completeRollback.record(true);
  completeRollback.record(true);
  assert(completeRollback.fullyRolledBack());

  CapsuleBrowserState state;
  assert(state.scope() == CapsuleScope::inbox);
  assert(!state.longPressReady(499, 0, 0));
  assert(state.longPressReady(500, 9, -9));
  assert(!state.longPressReady(500, 10, 0));
  assert(!state.toggle("capsule-b", false));
  assert(state.toggle("capsule-b", true));
  assert(state.selectionMode() && state.rootSwipeLocked());
  assert(state.selectedCount() == 1 && state.selected("capsule-b"));
  // Reordering visible rows cannot move selection to another capsule.
  std::vector<std::string> reordered = {"capsule-b", "capsule-a"};
  state.focus("capsule-b");
  state.retainKnown(reordered);
  state.retainFocused(reordered);
  assert(state.selected("capsule-b"));
  assert(!state.selected("capsule-a"));
  assert(state.focusedId() == "capsule-b");
  // A capsule filtered out or removed is trimmed safely.
  state.retainKnown({"capsule-a"});
  state.retainFocused({"capsule-a"});
  assert(!state.selectionMode());
  assert(state.selectedCount() == 0);
  assert(state.focusedId().empty());
  assert(state.toggle("capsule-b", true));
  assert(state.toggle("capsule-b", true));
  state.beginUndo(UINT32_MAX - 100);
  assert(state.undoAvailable(UINT32_MAX - 50));
  assert(state.undoAvailable(200));
  assert(!state.undoAvailable(5000));
  state.setOffset(4);
  state.setScope(CapsuleScope::archive);
  assert(state.offset() == 0 && state.scope() == CapsuleScope::archive);
  return 0;
}
