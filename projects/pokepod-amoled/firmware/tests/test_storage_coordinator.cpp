#include <atomic>
#include <cassert>
#include <thread>

#include "../PokePodAmoled/StorageCoordinator.cpp"

using namespace pokepod;

int main() {
  StorageCoordinator coordinator;
  {
    StorageReservation recorder = coordinator.reserve(
        StorageOwner::recorder, StorageAccess::mutation);
    assert(recorder);
    assert(coordinator.mutationActive());
    assert(coordinator.mutationOwner() == StorageOwner::recorder);
    assert(!coordinator.reserve(StorageOwner::tencentRead,
                                StorageAccess::read));
    assert(!coordinator.acquireIo(StorageOwner::usbLink,
                                  StorageAccess::mutation));
    StorageIoLease write = coordinator.acquireIo(
        StorageOwner::recorder, StorageAccess::mutation);
    assert(write);
  }
  assert(!coordinator.mutationActive());

  StorageReservation asr = coordinator.reserve(
      StorageOwner::tencentRead, StorageAccess::read);
  assert(asr);
  assert(coordinator.readOwner() == StorageOwner::tencentRead);
  {
    StorageIoLease read = coordinator.acquireIo(
        StorageOwner::tencentRead, StorageAccess::read);
    assert(read);
  }
  assert(!coordinator.reserve(StorageOwner::capsuleTransaction,
                              StorageAccess::mutation));
  asr.release();
  assert(coordinator.readOwner() == StorageOwner::none);

  StorageReservation outer = coordinator.reserve(
      StorageOwner::capsuleTransaction, StorageAccess::mutation);
  StorageReservation nested = coordinator.reserve(
      StorageOwner::capsuleTransaction, StorageAccess::mutation);
  assert(outer && nested);
  nested.release();
  assert(coordinator.mutationActive());
  outer.release();
  assert(!coordinator.mutationActive());

  std::atomic<bool> entered{false};
  StorageIoLease held = coordinator.acquireIo(
      StorageOwner::recovery, StorageAccess::mutation);
  assert(held);
  std::thread contender([&]() {
    StorageIoLease lease = coordinator.acquireIo(
        StorageOwner::capsuleTransaction, StorageAccess::mutation, 5);
    entered.store(static_cast<bool>(lease));
  });
  contender.join();
  assert(!entered.load());
  held.release();

  StorageReservation ownerHeld = coordinator.reserve(
      StorageOwner::capsuleTransaction, StorageAccess::mutation);
  assert(ownerHeld);
  entered.store(true);
  std::thread sameOwnerContender([&]() {
    StorageReservation lease = coordinator.reserve(
        StorageOwner::capsuleTransaction, StorageAccess::mutation, 5);
    entered.store(static_cast<bool>(lease));
  });
  sameOwnerContender.join();
  assert(!entered.load());
  ownerHeld.release();
  assert(!coordinator.mutationActive());

  StorageReservation readHeld = coordinator.reserve(
      StorageOwner::tencentRead, StorageAccess::read);
  assert(readHeld);
  entered.store(true);
  std::thread sameReadOwnerContender([&]() {
    StorageReservation lease = coordinator.reserve(
        StorageOwner::tencentRead, StorageAccess::read, 5);
    entered.store(static_cast<bool>(lease));
  });
  sameReadOwnerContender.join();
  assert(!entered.load());
  readHeld.release();
  assert(coordinator.readOwner() == StorageOwner::none);
  assert(coordinator.metrics().rejected >= 3);
  return 0;
}
