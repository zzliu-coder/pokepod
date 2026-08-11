#include <assert.h>

#include <atomic>
#include <thread>

#include "../PokePodAmoled/StorageCoordinator.cpp"
#include "../PokePodAmoled/DeferredFileCleanup.h"
#include "../PokePodAmoled/MaintenanceCompletionTracker.h"

using namespace pokepod;

namespace {

void testOutgoingDisconnectWaitsForPhysicalIoAndPreservesCompletionOrder() {
  StorageCoordinator &coordinator = StorageCoordinator::instance();
  fs::FS fs;
  fs.state()->seed("/PokeCapsule/Inbox/id/audio.wav", "audio");
  File file = fs.open("/PokeCapsule/Inbox/id/audio.wav", FILE_READ);
  assert(file);
  StorageReservation reservation = coordinator.reserve(
      StorageOwner::usbLink, StorageAccess::read);
  assert(reservation);

  std::atomic<bool> leaseReady{false};
  std::atomic<bool> releaseLease{false};
  std::thread otherTask([&]() {
    StorageIoLease lease = coordinator.acquireIo(
        StorageOwner::fontRead, StorageAccess::read, 50);
    assert(lease);
    leaseReady.store(true);
    while (!releaseLease.load()) std::this_thread::yield();
  });
  while (!leaseReady.load()) std::this_thread::yield();

  DeferredFileCleanup cleanup;
  assert(cleanup.begin(file, nullptr, "", reservation,
                       StorageOwner::usbLink, StorageAccess::read));
  MaintenanceCompletionTracker completion;
  static constexpr const char *kResult =
      "22222222-2222-4222-8222-222222222222";
  completion.beginAccepted();
  completion.endResultPersisted(kResult);

  // This is the production disconnect/final-frame cleanup condition: another
  // task owns physical IO, so no close, reservation release, or confirmed
  // completion may occur in this poll.
  assert(!cleanup.poll());
  assert(cleanup.pending());
  assert(file);
  assert(reservation);
  assert(completion.completionRevision() == 0);

  releaseLease.store(true);
  otherTask.join();
  assert(cleanup.poll());
  assert(!cleanup.pending());
  assert(!file);
  assert(!reservation);
  assert(completion.resultFetched(kResult, true));
  assert(completion.completionRevision() == 1);

  // Once deferred cleanup releases the reservation, either transport can own
  // the next session.
  StorageReservation next = coordinator.reserve(
      StorageOwner::wifiLink, StorageAccess::read);
  assert(next);
}

void testIncomingTemporaryRemovalRetainsMutationReservationUntilComplete() {
  StorageCoordinator &coordinator = StorageCoordinator::instance();
  fs::FS fs;
  const String path = "/PokeCapsule/.staging/id/audio.wav.part";
  fs.state()->seed(path.c_str(), "partial");
  File file = fs.open(path, FILE_READ);
  assert(file);
  StorageReservation reservation = coordinator.reserve(
      StorageOwner::usbLink, StorageAccess::mutation);
  assert(reservation);

  DeferredFileCleanup cleanup;
  assert(cleanup.begin(file, &fs, path, reservation,
                       StorageOwner::usbLink, StorageAccess::mutation));
  fs.state()->fail(fakefs::Operation::remove, 1,
                   fakefs::FaultAction::returnFailure);
  assert(!cleanup.poll());
  assert(cleanup.pending());
  assert(!file);
  assert(reservation);
  assert(fs.exists(path));
  assert(coordinator.mutationActive());

  fs.state()->clearFault();
  assert(cleanup.poll());
  assert(!cleanup.pending());
  assert(!reservation);
  assert(!fs.exists(path));
  assert(!coordinator.mutationActive());

  StorageReservation next = coordinator.reserve(
      StorageOwner::wifiLink, StorageAccess::mutation);
  assert(next);
}

}  // namespace

int main() {
  testOutgoingDisconnectWaitsForPhysicalIoAndPreservesCompletionOrder();
  testIncomingTemporaryRemovalRetainsMutationReservationUntilComplete();
  return 0;
}
