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
  for (size_t index = 0; index < 3; ++index) {
    const auto work = executor.poll(true);
    assert(work.action == LinkCommandExecutor::Action::applyItem);
    assert(work.index == index);
    executor.completeStep(index != 2);
  }
  assert(executor.applied() == 2);
  for (size_t index = 2; index > 0; --index) {
    const auto work = executor.poll(false);
    assert(work.action == LinkCommandExecutor::Action::rollbackItem);
    assert(work.index == index - 1);
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
  executor.completeStep(executor.poll(true).index == 0);
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

  LinkCommandExecutor tooLarge;
  assert(!tooLarge.begin(LinkCommandExecutor::kMaximumItems + 1));

  LinkCommandExecutor recovered;
  assert(recovered.resume(5, LinkCommandExecutor::Phase::rollback,
                          3, 3, false, false));
  work = recovered.poll(false);
  assert(work.action == LinkCommandExecutor::Action::rollbackItem);
  assert(work.index == 2);
  return 0;
}
