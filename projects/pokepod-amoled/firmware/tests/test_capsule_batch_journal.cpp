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
  first.applied = 37;
  first.phase = CapsuleBatchPhase::rollback;
  first.generation = 9;
  sealCapsuleBatchState(first);
  assert(validCapsuleBatchState(first));

  StoredCapsuleBatchState second = first;
  second.generation = 10;
  second.cursor = 11;
  sealCapsuleBatchState(second);
  assert(newestCapsuleBatchState(first, second) == &second);

  // Failed inactive-slot writes never advance the durable generation, so all
  // retries target the same slot and cannot overwrite the last valid slot.
  StoredCapsuleBatchState durable = first;
  const StoredCapsuleBatchState candidate =
      nextCapsuleBatchCheckpoint(durable);
  StoredCapsuleBatchState corrupt = candidate;
  corrupt.crc32 ^= 1;
  assert(!acceptCapsuleBatchCheckpoint(durable, candidate, corrupt));
  assert(durable.generation == first.generation);
  assert(!acceptCapsuleBatchCheckpoint(durable, candidate, corrupt));
  assert(durable.generation == first.generation);
  assert(acceptCapsuleBatchCheckpoint(durable, candidate, candidate));
  assert(durable.generation == candidate.generation);

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
  assert(validCapsuleBatchPlan(plan, "moveCapsules"));
  plan.target[3] ^= 1;
  assert(!validCapsuleBatchPlan(plan));

  StoredCapsuleBatchState malicious = first;
  memset(malicious.transactionId, 'a', sizeof(malicious.transactionId));
  sealCapsuleBatchState(malicious);
  assert(!validCapsuleBatchState(malicious));
  malicious = first;
  strcpy(malicious.operation, "unknownOperation");
  sealCapsuleBatchState(malicious);
  assert(!validCapsuleBatchState(malicious));
  malicious = first;
  malicious.flags = 0x80;
  sealCapsuleBatchState(malicious);
  assert(!validCapsuleBatchState(malicious));
  malicious = first;
  malicious.phase = CapsuleBatchPhase::rollback;
  malicious.cursor = 4;
  malicious.applied = 3;
  sealCapsuleBatchState(malicious);
  assert(!validCapsuleBatchState(malicious));

  StoredCapsuleBatchPlan escaped;
  strcpy(escaped.id, "22222222-2222-4222-8222-222222222222");
  strcpy(escaped.source, "/PokeCapsule/Inbox/../../secret");
  strcpy(escaped.target, "/PokeCapsule/Archive/x");
  sealCapsuleBatchPlan(escaped);
  assert(!validCapsuleBatchPlan(escaped, "moveCapsules"));
  memset(escaped.source, 'x', sizeof(escaped.source));
  sealCapsuleBatchPlan(escaped);
  assert(!validCapsuleBatchPlan(escaped, "moveCapsules"));

  StoredCapsuleBatchPlan protectedPath;
  strcpy(protectedPath.id, "22222222-2222-4222-8222-222222222222");
  strcpy(protectedPath.source,
         "/PokeCapsule/.system/22222222-2222-4222-8222-222222222222");
  strcpy(protectedPath.target,
         "/PokeCapsule/Inbox/22222222-2222-4222-8222-222222222222");
  sealCapsuleBatchPlan(protectedPath);
  assert(!validCapsuleBatchPlan(protectedPath, "moveCapsules"));
  strcpy(protectedPath.source,
         "/PokeCapsule/.staging/22222222-2222-4222-8222-222222222222");
  sealCapsuleBatchPlan(protectedPath);
  assert(!validCapsuleBatchPlan(protectedPath, "moveCapsules"));
  strcpy(protectedPath.source, "/PokeCapsule/Inbox");
  sealCapsuleBatchPlan(protectedPath);
  assert(!validCapsuleBatchPlan(protectedPath, "moveCapsules"));

  StoredCapsuleBatchPlan trashPlan;
  strcpy(trashPlan.id, "22222222-2222-4222-8222-222222222222");
  strcpy(trashPlan.source,
         "/PokeCapsule/Inbox/22222222-2222-4222-8222-222222222222");
  strcpy(trashPlan.target,
         "/PokeCapsule/.trash/22222222-2222-4222-8222-222222222222");
  sealCapsuleBatchPlan(trashPlan);
  assert(validCapsuleBatchPlan(trashPlan, "deleteCapsules"));
  assert(!validCapsuleBatchPlan(trashPlan, "restoreCapsules"));
  strcpy(trashPlan.source,
         "/PokeCapsule/.trash/22222222-2222-4222-8222-222222222222");
  strcpy(trashPlan.target,
         "/PokeCapsule/Inbox/22222222-2222-4222-8222-222222222222");
  sealCapsuleBatchPlan(trashPlan);
  assert(validCapsuleBatchPlan(trashPlan, "restoreCapsules"));
  assert(!validCapsuleBatchPlan(trashPlan, "deleteCapsules"));

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
