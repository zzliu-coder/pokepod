#include <cassert>
#include <cstdint>
#include <cstring>

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
  char longestExistingPath[231];
  memset(longestExistingPath, 'a', sizeof(longestExistingPath));
  longestExistingPath[0] = '/';
  longestExistingPath[sizeof(longestExistingPath) - 1] = '\0';
  assert(setCapsuleTransactionTarget(
      journal.targets[0], longestExistingPath, 1, 2, false));

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
