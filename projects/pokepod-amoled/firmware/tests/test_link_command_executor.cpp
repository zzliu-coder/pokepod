#include <assert.h>

#include "../PokePodAmoled/LinkCommandExecutor.h"

using pokepod::LinkCommandExecutor;

int main() {
  LinkCommandExecutor executor;
  assert(executor.begin(3));
  for (size_t index = 0; index < 3; ++index) {
    const auto work = executor.poll(true);
    assert(work.action == LinkCommandExecutor::Action::preflightItem);
    assert(work.index == index);
    executor.completeStep(true);
  }
  auto checkpoint = executor.poll(true);
  assert(checkpoint.action == LinkCommandExecutor::Action::checkpoint);
  executor.completeStep(true);
  for (size_t index = 0; index < 3; ++index) {
    checkpoint = executor.poll(true);
    assert(checkpoint.action == LinkCommandExecutor::Action::checkpoint);
    assert(checkpoint.index == index);
    executor.completeStep(true);
    const auto work = executor.poll(true);
    assert(work.action == LinkCommandExecutor::Action::applyItem);
    assert(work.index == index);
    executor.completeStep(index != 2);
    checkpoint = executor.poll(true);
    assert(checkpoint.action == LinkCommandExecutor::Action::checkpoint);
    executor.completeStep(true);
  }
  // The third item failed after its adapter was entered, so it is treated as
  // an uncertain mutation and included in live rollback.
  assert(executor.applied() == 3);
  for (size_t index = 3; index > 0; --index) {
    checkpoint = executor.poll(false);
    assert(checkpoint.action == LinkCommandExecutor::Action::checkpoint);
    executor.completeStep(true);
    const auto work = executor.poll(false);
    assert(work.action == LinkCommandExecutor::Action::rollbackItem);
    assert(work.index == index - 1);
    executor.completeStep(true);
    checkpoint = executor.poll(false);
    assert(checkpoint.action == LinkCommandExecutor::Action::checkpoint);
    executor.completeStep(true);
  }
  auto work = executor.poll(false);
  assert(work.action == LinkCommandExecutor::Action::persistResult);
  executor.completeStep(true);
  work = executor.poll(false);
  assert(work.action == LinkCommandExecutor::Action::cleanup);
  executor.completeStep(true);
  work = executor.poll(false);
  assert(work.action == LinkCommandExecutor::Action::finish);
  assert(!executor.responseAllowed());
  executor.completeStep(true);
  assert(!executor.active());

  // A deadline crossed during apply immediately changes the next unit of work
  // to reverse-order rollback.  Local recovery is allowed to continue while
  // the transport remains cancelled.
  assert(executor.begin(2));
  executor.completeStep(executor.poll(true).index == 0);
  executor.completeStep(executor.poll(true).index == 1);
  executor.completeStep(executor.poll(true).action ==
                        LinkCommandExecutor::Action::checkpoint);
  executor.completeStep(executor.poll(true).action ==
                        LinkCommandExecutor::Action::checkpoint);
  executor.completeStep(executor.poll(true).index == 0);
  executor.completeStep(executor.poll(true).action ==
                        LinkCommandExecutor::Action::checkpoint);
  work = executor.poll(false);
  assert(work.action == LinkCommandExecutor::Action::checkpoint);
  executor.completeStep(true);
  work = executor.poll(false);
  assert(work.action == LinkCommandExecutor::Action::rollbackItem);
  assert(work.index == 0);
  assert(executor.deadlineObserved());
  executor.completeStep(true);

  // Disconnect suppresses the response without discarding the command owner.
  executor.reset();
  assert(executor.begin(1));
  executor.disconnect();
  assert(executor.active());
  assert(!executor.responseAllowed());

  // A USB CDC close is a foreground cancellation even though USB remains
  // physically mounted and has no absolute transfer deadline.  The adapter
  // passes sessionActive && transferPermitted, so the next poll rolls back.
  executor.reset();
  assert(executor.begin(2));
  executor.completeStep(executor.poll(true).index == 0);
  executor.completeStep(executor.poll(true).index == 1);
  executor.completeStep(executor.poll(true).action ==
                        LinkCommandExecutor::Action::checkpoint);
  executor.completeStep(executor.poll(true).action ==
                        LinkCommandExecutor::Action::checkpoint);
  executor.completeStep(executor.poll(true).action ==
                        LinkCommandExecutor::Action::applyItem);
  executor.completeStep(executor.poll(true).action ==
                        LinkCommandExecutor::Action::checkpoint);
  executor.disconnect();
  work = executor.poll(false);
  assert(work.action == LinkCommandExecutor::Action::checkpoint);
  executor.completeStep(true);
  work = executor.poll(false);
  assert(work.action == LinkCommandExecutor::Action::rollbackItem);
  assert(work.index == 0);
  assert(!executor.responseAllowed());

  LinkCommandExecutor tooLarge;
  assert(!tooLarge.begin(LinkCommandExecutor::kMaximumItems + 1));

  LinkCommandExecutor recovered;
  assert(recovered.resume(5, LinkCommandExecutor::Phase::rollback,
                          3, 3, false, false));
  work = recovered.poll(false);
  assert(work.action == LinkCommandExecutor::Action::checkpoint);
  recovered.completeStep(true);
  work = recovered.poll(false);
  assert(work.action == LinkCommandExecutor::Action::rollbackItem);
  assert(work.index == 2);

  // A permanently failing artifact cleanup reaches a fail-closed terminal.
  // The caller can release runtime ownership while keeping the durable
  // journal for boot recovery.
  LinkCommandExecutor blockedCleanup;
  assert(blockedCleanup.resume(1, LinkCommandExecutor::Phase::cleanup,
                               0, 0, false, false));
  for (int attempt = 0; attempt < 3; ++attempt) {
    work = blockedCleanup.poll(false);
    assert(work.action == LinkCommandExecutor::Action::cleanup);
    blockedCleanup.completeStep(false);
  }
  assert(blockedCleanup.preserveJournal());
  work = blockedCleanup.poll(false);
  assert(work.action == LinkCommandExecutor::Action::finish);
  blockedCleanup.completeStep(true);
  assert(!blockedCleanup.active());

  // Folder finalization is an atomic artifact of its own.  A power cut after
  // the rename but before its post-checkpoint resumes by restoring that
  // artifact before any capsule item is rolled back.
  LinkCommandExecutor folder;
  assert(folder.begin(0, true));
  work = folder.poll(true);
  assert(work.action == LinkCommandExecutor::Action::checkpoint);
  folder.completeStep(true);
  work = folder.poll(true);
  assert(work.action == LinkCommandExecutor::Action::finalizeApply);
  folder.completeStep(true);
  assert(folder.finalizeApplied());
  // Simulate three failed writes of the post-finalize checkpoint.
  for (int attempt = 0; attempt < 3; ++attempt) {
    work = folder.poll(true);
    assert(work.action == LinkCommandExecutor::Action::checkpoint);
    folder.completeStep(false);
  }
  work = folder.poll(false);
  assert(work.action == LinkCommandExecutor::Action::checkpoint);
  folder.completeStep(true);
  work = folder.poll(false);
  assert(work.action == LinkCommandExecutor::Action::rollbackFinalize);
  folder.completeStep(true);
  work = folder.poll(false);
  assert(work.action == LinkCommandExecutor::Action::checkpoint);
  folder.completeStep(true);
  assert(!folder.finalizeApplied());

  LinkCommandExecutor recoveredFolder;
  assert(recoveredFolder.resume(2, LinkCommandExecutor::Phase::rollback,
                                2, 2, false, false, true, true));
  work = recoveredFolder.poll(false);
  assert(work.action == LinkCommandExecutor::Action::checkpoint);
  recoveredFolder.completeStep(true);
  work = recoveredFolder.poll(false);
  assert(work.action == LinkCommandExecutor::Action::rollbackFinalize);
  return 0;
}
