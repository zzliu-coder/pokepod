#include <assert.h>
#include <string.h>

#include "../PokePodAmoled/CapsuleBatchJournal.h"

using namespace pokepod;

int main() {
  StoredCapsuleBatchState first;
  strcpy(first.transactionId, "11111111-1111-4111-8111-111111111111");
  strcpy(first.operation, "moveCapsules");
  first.total = 500;
  first.cursor = 37;
  first.applied = 12;
  first.phase = CapsuleBatchPhase::rollback;
  first.generation = 9;
  sealCapsuleBatchState(first);
  assert(validCapsuleBatchState(first));

  StoredCapsuleBatchState second = first;
  second.generation = 10;
  second.cursor = 11;
  sealCapsuleBatchState(second);
  assert(newestCapsuleBatchState(first, second) == &second);

  // A torn newest slot leaves the previous checkpoint authoritative.
  second.operation[0] ^= 1;
  assert(newestCapsuleBatchState(first, second) == &first);

  // Generation comparison remains valid across uint32 wrap.
  first.generation = 0xffffffffU;
  sealCapsuleBatchState(first);
  second = first;
  second.generation = 0;
  sealCapsuleBatchState(second);
  assert(newestCapsuleBatchState(first, second) == &second);

  StoredCapsuleBatchPlan plan;
  strcpy(plan.id, "22222222-2222-4222-8222-222222222222");
  strcpy(plan.targetId, "33333333-3333-4333-8333-333333333333");
  strcpy(plan.source, "/PokeCapsule/Inbox/22222222-2222-4222-8222-222222222222");
  strcpy(plan.target, "/PokeCapsule/Archive/33333333-3333-4333-8333-333333333333");
  plan.expectedRevision = 4;
  sealCapsuleBatchPlan(plan);
  assert(validCapsuleBatchPlan(plan));
  plan.target[3] ^= 1;
  assert(!validCapsuleBatchPlan(plan));

  // A cut after an apply mutation but before its post-checkpoint includes the
  // uncertain item in reverse recovery. Repeating recovery is idempotent.
  StoredCapsuleBatchState cut = first;
  cut.generation = 12;
  cut.total = 5;
  cut.cursor = 2;
  cut.applied = 2;
  cut.phase = CapsuleBatchPhase::apply;
  cut.flags = capsuleBatchResponseAllowed | capsuleBatchItemInFlight;
  sealCapsuleBatchState(cut);
  prepareCapsuleBatchRecovery(cut);
  assert(cut.phase == CapsuleBatchPhase::rollback);
  assert(cut.cursor == 3 && cut.applied == 3);
  assert((cut.flags & capsuleBatchResponseAllowed) == 0);
  const StoredCapsuleBatchState once = cut;
  prepareCapsuleBatchRecovery(cut);
  assert(memcmp(&once, &cut, sizeof(cut)) == 0);

  // A cut inside rollback retries the same reverse item.
  cut.cursor = 2;
  cut.applied = 3;
  cut.phase = CapsuleBatchPhase::rollback;
  cut.flags |= capsuleBatchItemInFlight;
  sealCapsuleBatchState(cut);
  prepareCapsuleBatchRecovery(cut);
  assert(cut.phase == CapsuleBatchPhase::rollback);
  assert(cut.cursor == 2);
  return 0;
}
