#include <cassert>
#include <cstdint>
#include <cstring>
#include <initializer_list>

#include "CapsuleTransactionPolicy.h"

using namespace pokepod;

namespace {

uint32_t nextRandom(uint32_t &state) {
  state ^= state << 13;
  state ^= state >> 17;
  state ^= state << 5;
  return state;
}

bool canCommit(const CapsuleTransactionTargetFacts &target) {
  return target.targetMatchesNew || target.newMatchesExpected;
}

bool canRollback(const CapsuleTransactionTargetFacts &target) {
  if (target.hadOriginal) {
    return target.backupExists ||
        (target.targetExists && !target.targetMatchesNew);
  }
  return !target.targetExists || target.targetMatchesNew;
}

}  // namespace

int main() {
  StoredCapsuleTransactionJournal journal{};
  journal.magic = kCapsuleTransactionMagic;
  journal.version = kCapsuleTransactionVersion;
  journal.targetCount = 2;
  assert(setCapsuleTransactionTarget(
      journal.targets[0], "/PokeCapsule/Inbox/id/raw.txt", 3,
      capsuleTransactionCrc32(reinterpret_cast<const uint8_t *>("abc"), 3),
      false));
  assert(setCapsuleTransactionTarget(
      journal.targets[1], "/PokeCapsule/Inbox/id/processing.json", 2,
      capsuleTransactionCrc32(reinterpret_cast<const uint8_t *>("{}"), 2),
      true));
  finalizeCapsuleTransactionJournal(journal);
  assert(validateCapsuleTransactionJournal(journal));
  StoredCapsuleTransactionJournal corrupt = journal;
  corrupt.targets[0].expectedLength++;
  assert(!validateCapsuleTransactionJournal(corrupt));
  assert(!capsuleTransactionPathValid("relative/path"));
  assert(!capsuleTransactionPathValid("/PokeCapsule/../secret"));
  assert(!capsuleTransactionTargetPathValid("/foo"));
  assert(!capsuleTransactionTargetPathValid("/PokeCapsule/raw.txt"));
  assert(!capsuleTransactionTargetPathValid(
      "/PokeCapsule/.system/transactions/tx-1234.journal"));
  assert(!capsuleTransactionTargetPathValid(
      "/PokeCapsule/Inbox/id/raw.txt.bak"));
  assert(capsuleTransactionTargetPathValid(
      "/PokeCapsule/.system/fonts/cjk20.a4"));
  assert(capsuleTransactionTargetPathValid(
      "/PokeCapsule/.system/commands/incoming/123.json"));
  assert(capsuleTransactionTargetPathValid(
      "/PokeCapsule/.system/commands/results/123.json"));
  assert(!capsuleTransactionTargetPathValid(
      "/PokeCapsule/.system/arbitrary/value.json"));
  assert(capsuleTransactionPreparedPathValid(
      "/PokeCapsule/.staging/id/audio.wav.part"));
  assert(!capsuleTransactionPreparedPathValid(
      "/PokeCapsule/Inbox/id/audio.wav.part"));
  assert(capsuleTransactionPreparedPathValid(
      "/PokeCapsule/.system/fonts/cjk20.a4.part",
      "/PokeCapsule/.system/fonts/cjk20.a4"));
  assert(capsuleTransactionPreparedPathValid(
      "/PokeCapsule/.system/commands/incoming/123.json.part",
      "/PokeCapsule/.system/commands/incoming/123.json"));
  assert(!capsuleTransactionPreparedPathValid(
      "/PokeCapsule/.system/transactions/tx.part",
      "/PokeCapsule/.system/transactions/tx"));
  char longestExistingPath[231] = "/PokeCapsule/Inbox/";
  const size_t prefixLength = strlen(longestExistingPath);
  memset(longestExistingPath + prefixLength, 'a',
         sizeof(longestExistingPath) - prefixLength - 9);
  memcpy(longestExistingPath + sizeof(longestExistingPath) - 9,
         "/raw.txt", 9);
  assert(setCapsuleTransactionTarget(
      journal.targets[0], longestExistingPath, 1, 2, false));

  for (const char *forgedPath : {
           "/foo",
           "/PokeCapsule/raw.txt",
           "/PokeCapsule/.system/transactions/tx-forged.journal",
       }) {
    StoredCapsuleTransactionJournal forged{};
    forged.magic = kCapsuleTransactionMagic;
    forged.version = kCapsuleTransactionVersion;
    forged.targetCount = 1;
    strcpy(forged.targets[0].path, forgedPath);
    forged.targets[0].expectedLength = 1;
    forged.targets[0].expectedCrc32 = 2;
    finalizeCapsuleTransactionJournal(forged);
    assert(!validateCapsuleTransactionJournal(forged));
  }

  CapsuleTransactionTargetFacts committed[2] = {
      {false, true, true, false, false},
      {true, true, true, false, true},
  };
  assert(decideCapsuleTransactionRecovery(committed, 2) ==
         CapsuleTransactionRecovery::alreadyCommitted);
  CapsuleTransactionTargetFacts forward[2] = {
      {false, true, true, false, false},
      {true, false, false, true, true},
  };
  assert(decideCapsuleTransactionRecovery(forward, 2) ==
         CapsuleTransactionRecovery::commitNew);
  CapsuleTransactionTargetFacts rollback[2] = {
      {false, false, false, false, false},
      {true, false, false, false, true},
  };
  assert(decideCapsuleTransactionRecovery(rollback, 2) ==
         CapsuleTransactionRecovery::rollbackOld);

  uint32_t random = 0x8f31a5c7U;
  for (size_t iteration = 0; iteration < 10000; ++iteration) {
    CapsuleTransactionTargetFacts targets[2]{};
    for (auto &target : targets) {
      const uint32_t bits = nextRandom(random);
      target.hadOriginal = (bits & 1U) != 0;
      target.targetExists = (bits & 2U) != 0;
      target.targetMatchesNew = target.targetExists && (bits & 4U) != 0;
      target.newMatchesExpected = (bits & 8U) != 0;
      target.backupExists = (bits & 16U) != 0;
    }
    const CapsuleTransactionRecovery decision =
        decideCapsuleTransactionRecovery(targets, 2);
    const bool forwardPossible = canCommit(targets[0]) && canCommit(targets[1]);
    const bool rollbackPossible =
        canRollback(targets[0]) && canRollback(targets[1]);
    if (decision == CapsuleTransactionRecovery::alreadyCommitted) {
      assert(targets[0].targetMatchesNew && targets[1].targetMatchesNew);
    } else if (decision == CapsuleTransactionRecovery::commitNew) {
      assert(forwardPossible);
    } else if (decision == CapsuleTransactionRecovery::rollbackOld) {
      assert(!forwardPossible && rollbackPossible);
    } else {
      assert(!forwardPossible && !rollbackPossible);
    }
  }
  return 0;
}
