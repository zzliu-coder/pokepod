#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>
#include <atomic>
#include <thread>

#include "../PokePodAmoled/LinkCommandBatchPolicy.h"
#include "../PokePodAmoled/LinkTransferGate.h"
#include "../PokePodAmoled/StorageCoordinator.cpp"

int main() {
  using namespace pokepod;

  std::vector<int> values{10, 20, 30};
  const std::vector<int> originals = values;
  std::vector<size_t> order;
  values[0] = 11;
  values[1] = 21;
  values[2] = 31;
  const LinkBatchRollbackResult restored = rollbackLinkBatch(
      values.size(), [&](size_t index) {
        order.push_back(index);
        values[index] = originals[index];
        return true;
      });
  assert(restored.ok());
  assert(restored.attempted == 3);
  assert(order == std::vector<size_t>({2, 1, 0}));
  assert(values == originals);

  const LinkBatchRollbackResult rollbackFault = rollbackLinkBatch(
      3, [](size_t index) { return index != 1; });
  assert(!rollbackFault.ok());
  assert(rollbackFault.attempted == 3);
  assert(rollbackFault.failed == 1);

  // The final apply step can fail after its rename succeeded. It is still an
  // applied journal entry and must be included in rollback.
  size_t applied = 1;
  bool failed = false;
  const bool finalTouchOk = false;
  if (!finalTouchOk) {
    ++applied;
    failed = true;
  }
  assert(failed && applied == 2);
  size_t restoredEntries = 0;
  const LinkBatchRollbackResult finalTouchRollback = rollbackLinkBatch(
      applied, [&](size_t) {
        ++restoredEntries;
        return true;
      });
  assert(finalTouchRollback.ok());
  assert(restoredEntries == 2);

  // A copy beginning at 299 seconds must consult the same absolute gate for
  // every 4 KiB step. The partial destination is journaled and removed when
  // the first step at the 300 second boundary is rejected.
  AbsoluteLinkDeadlineGate gate;
  gate.arm(0, 300000);
  bool partialTargetExists = false;
  size_t copiedChunks = 0;
  for (uint32_t nowMs = 299000; nowMs <= 301000; nowMs += 250) {
    if (!linkTransferPermitted(&gate, nowMs)) break;
    partialTargetExists = true;
    ++copiedChunks;
  }
  assert(copiedChunks == 4);
  assert(partialTargetExists);
  const LinkBatchRollbackResult copyRollback = rollbackLinkBatch(
      1, [&](size_t) {
        partialTargetExists = false;
        return true;
      });
  assert(copyRollback.ok());
  assert(!partialTargetExists);

  class CancelProbe final : public LinkTransferCancellationSink {
   public:
    void cancelForTransferDeadline() override { ++calls; }
    size_t calls = 0;
  } cancelProbe;
  AbsoluteLinkDeadlineGate rollbackGate;
  rollbackGate.attachCancellationSink(&cancelProbe);
  rollbackGate.arm(0, 300000);
  uint32_t rollbackNow = 299900;
  size_t localCleanupSteps = 0;
  const LinkBatchRollbackResult deadlineRollback = rollbackLinkBatch(
      5, [&](size_t) {
        (void)rollbackGate.permits(rollbackNow);
        rollbackNow += 75;
        ++localCleanupSteps;
        (void)rollbackGate.permits(rollbackNow);
        return true;
      });
  assert(deadlineRollback.ok());
  assert(localCleanupSteps == 5);
  assert(cancelProbe.calls == 1);

  // A deferred command File keeps the command's logical mutation reservation
  // until its physical close succeeds. A foreign owner cannot enter between
  // those two events; it can acquire immediately after release.
  StorageCoordinator coordinator;
  StorageReservation command = coordinator.reserve(
      StorageOwner::capsuleTransaction, StorageAccess::mutation);
  assert(command);
  std::atomic<bool> foreignEntered{true};
  std::thread blocked([&]() {
    StorageIoLease foreign = coordinator.acquireIo(
        StorageOwner::wifiLink, StorageAccess::mutation, 5);
    foreignEntered.store(static_cast<bool>(foreign));
  });
  blocked.join();
  assert(!foreignEntered.load());
  {
    StorageIoLease closeLease = coordinator.acquireIo(
        StorageOwner::capsuleTransaction, StorageAccess::mutation);
    assert(closeLease);
  }
  assert(coordinator.mutationOwner() == StorageOwner::capsuleTransaction);
  command.release();
  StorageIoLease next = coordinator.acquireIo(
      StorageOwner::wifiLink, StorageAccess::mutation);
  assert(next);
  return 0;
}
