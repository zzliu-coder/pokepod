#include <assert.h>
#include <string.h>

#include "../PokePodAmoled/StorageCoordinator.cpp"
#include "../PokePodAmoled/CapsuleBatchJournalStore.cpp"

using namespace pokepod;

namespace {

constexpr const char *kTransaction =
    "11111111-1111-4111-8111-111111111111";

void assertInitialStillLoads(CapsuleBatchJournalStore &store,
                             const StoredCapsuleBatchState &initial) {
  StoredCapsuleBatchState loaded;
  assert(store.load(kTransaction, StorageOwner::recovery, loaded));
  assert(loaded.generation == initial.generation);
  assert(loaded.phase == initial.phase);
  assert(loaded.cursor == initial.cursor);
}

void checkpointFaultKeepsOnlyValidSlot(fakefs::Operation operation,
                                       fakefs::FaultAction action) {
  fs::FS fs;
  CapsuleBatchJournalStore store;
  assert(store.begin(fs));
  StoredCapsuleBatchState state;
  assert(store.create(kTransaction, "moveCapsules", 2,
                      StorageOwner::recovery, state));
  const StoredCapsuleBatchState initial = state;
  state.phase = CapsuleBatchPhase::apply;
  state.cursor = state.applied = 1;
  sealCapsuleBatchState(state);

  fs.state()->fail(operation, 1, action);
  assert(!store.checkpoint(state, StorageOwner::recovery));
  assert(state.generation == initial.generation);
  fs.state()->clearFault();
  assertInitialStillLoads(store, initial);

  // Every failed retry targets the same inactive slot.  A later successful
  // checkpoint advances once without ever touching the old authority.
  fs.state()->fail(operation, 1, action);
  assert(!store.checkpoint(state, StorageOwner::recovery));
  fs.state()->clearFault();
  assertInitialStillLoads(store, initial);
  assert(store.checkpoint(state, StorageOwner::recovery));
  assert(state.generation == initial.generation + 1);
  StoredCapsuleBatchState loaded;
  assert(store.load(kTransaction, StorageOwner::recovery, loaded));
  assert(loaded.generation == state.generation);
}

}  // namespace

int main() {
  checkpointFaultKeepsOnlyValidSlot(fakefs::Operation::write,
                                    fakefs::FaultAction::shortWrite);
  checkpointFaultKeepsOnlyValidSlot(fakefs::Operation::flush,
                                    fakefs::FaultAction::returnFailure);
  checkpointFaultKeepsOnlyValidSlot(fakefs::Operation::close,
                                    fakefs::FaultAction::returnFailure);
  checkpointFaultKeepsOnlyValidSlot(fakefs::Operation::read,
                                    fakefs::FaultAction::shortRead);

  // Public entry points reject traversal before touching the filesystem.
  fs::FS fs;
  CapsuleBatchJournalStore store;
  assert(store.begin(fs));
  StoredCapsuleBatchState state;
  const uint32_t operations = fs.state()->operations;
  assert(!store.create("../../escape", "moveCapsules", 1,
                       StorageOwner::recovery, state));
  assert(fs.state()->operations == operations);
  assert(!store.create(kTransaction, "unknownOperation", 1,
                       StorageOwner::recovery, state));
  assert(fs.state()->operations == operations);

  // Contention is retryable and cannot be mistaken for a corrupt authority.
  assert(store.create(kTransaction, "moveCapsules", 1,
                      StorageOwner::recovery, state));
  StorageReservation foreign = StorageCoordinator::instance().reserve(
      StorageOwner::audioPlayback, StorageAccess::mutation, 0);
  assert(foreign);
  StoredCapsuleBatchState blocked;
  assert(store.loadStatus(kTransaction, StorageOwner::recovery, blocked) ==
         CapsuleBatchJournalStore::LoadResult::wouldBlock);
  foreign.release();
  assert(store.loadStatus(kTransaction, StorageOwner::recovery, blocked) ==
         CapsuleBatchJournalStore::LoadResult::loaded);
  return 0;
}
