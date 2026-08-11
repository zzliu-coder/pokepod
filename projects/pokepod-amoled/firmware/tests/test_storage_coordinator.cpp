#include <atomic>
#include <cassert>
#include <thread>

#include "../PokePodAmoled/PowerPolicy.h"
#include "../PokePodAmoled/StorageCoordinator.cpp"

using namespace pokepod;

namespace {

PowerDecision powerDecisionFor(const StorageCoordinator &coordinator) {
  PowerInputs input;
  input.screenOn = false;
  input.automaticWakeEnabled = false;
  input.idleMs = kDeepSleepTimeoutMs;
  PowerFacts facts;
  facts.storageMutationActive = coordinator.mutationActive();
  return decidePower(powerInputsWithFacts(input, facts));
}

}  // namespace

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

  // Read reservations never become power-blocking storage mutations.  This
  // covers cloud reads, playback, font glyph loading and Link downloads.
  const StorageOwner readOwners[] = {
      StorageOwner::tencentRead,
      StorageOwner::audioPlayback,
      StorageOwner::fontRead,
      StorageOwner::usbLink,
      StorageOwner::wifiLink,
  };
  for (const StorageOwner owner : readOwners) {
    StorageReservation readReservation = coordinator.reserve(
        owner, StorageAccess::read);
    assert(readReservation);
    assert(coordinator.readOwner() == owner);
    assert(!coordinator.mutationActive());
    assert(powerDecisionFor(coordinator).requestDeepSleep);
    {
      StorageIoLease read = coordinator.acquireIo(owner, StorageAccess::read);
      assert(read);
      assert(!coordinator.mutationActive());
    }
    assert(!coordinator.reserve(StorageOwner::capsuleTransaction,
                                StorageAccess::mutation));
    readReservation.release();
    assert(coordinator.readOwner() == StorageOwner::none);
    assert(!coordinator.mutationActive());
  }

  // Every logical write owner blocks sleep for its whole reservation, then
  // releases the fact immediately when the transaction is finished.
  const StorageOwner mutationOwners[] = {
      StorageOwner::recorder,
      StorageOwner::capsuleTransaction,
      StorageOwner::usbLink,
      StorageOwner::wifiLink,
      StorageOwner::recovery,
  };
  for (const StorageOwner owner : mutationOwners) {
    StorageReservation mutation = coordinator.reserve(
        owner, StorageAccess::mutation);
    assert(mutation);
    assert(coordinator.mutationActive());
    assert(coordinator.mutationOwner() == owner);
    assert(!powerDecisionFor(coordinator).requestDeepSleep);
    mutation.release();
    assert(!coordinator.mutationActive());
    assert(coordinator.mutationOwner() == StorageOwner::none);
    assert(powerDecisionFor(coordinator).requestDeepSleep);
  }

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

  // The power fact is exact across tasks. A long read holds the physical
  // storage mutex but never appears as a mutation; a write lease does.
  std::atomic<bool> leaseReady{false};
  std::atomic<bool> releaseLease{false};
  std::thread readIoHolder([&]() {
    StorageIoLease lease = coordinator.acquireIo(
        StorageOwner::fontRead, StorageAccess::read, 5);
    assert(lease);
    leaseReady.store(true);
    while (!releaseLease.load()) std::this_thread::yield();
  });
  while (!leaseReady.load()) std::this_thread::yield();
  assert(!coordinator.mutationActive());
  assert(powerDecisionFor(coordinator).requestDeepSleep);
  releaseLease.store(true);
  readIoHolder.join();

  leaseReady.store(false);
  releaseLease.store(false);
  std::thread mutationIoHolder([&]() {
    StorageIoLease lease = coordinator.acquireIo(
        StorageOwner::recovery, StorageAccess::mutation, 5);
    assert(lease);
    leaseReady.store(true);
    while (!releaseLease.load()) std::this_thread::yield();
  });
  while (!leaseReady.load()) std::this_thread::yield();
  assert(coordinator.mutationActive());
  assert(!powerDecisionFor(coordinator).requestDeepSleep);
  releaseLease.store(true);
  mutationIoHolder.join();
  assert(!coordinator.mutationActive());
  assert(powerDecisionFor(coordinator).requestDeepSleep);

  assert(coordinator.metrics().rejected >= 3);
  return 0;
}
