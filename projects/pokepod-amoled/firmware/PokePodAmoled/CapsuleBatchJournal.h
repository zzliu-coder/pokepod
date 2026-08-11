#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "CapsuleTransactionPolicy.h"

namespace pokepod {

constexpr uint32_t kCapsuleBatchJournalMagic = 0x324a4243U;  // CBJ2
constexpr uint16_t kCapsuleBatchJournalVersion = 1;
constexpr size_t kCapsuleBatchPathBytes = 224;

enum class CapsuleBatchPhase : uint8_t {
  preflight = 1,
  apply = 2,
  rollback = 3,
  result = 4,
  cleanup = 5,
};

enum CapsuleBatchFlags : uint8_t {
  capsuleBatchSuccess = 1U << 0,
  capsuleBatchResponseAllowed = 1U << 1,
  capsuleBatchRollbackFailed = 1U << 2,
  capsuleBatchItemInFlight = 1U << 3,
};

#pragma pack(push, 1)
struct StoredCapsuleBatchState {
  uint32_t magic = kCapsuleBatchJournalMagic;
  uint16_t version = kCapsuleBatchJournalVersion;
  uint16_t bytes = sizeof(StoredCapsuleBatchState);
  uint32_t generation = 0;
  char transactionId[37] = {};
  char operation[24] = {};
  uint16_t total = 0;
  uint16_t cursor = 0;
  uint16_t applied = 0;
  CapsuleBatchPhase phase = CapsuleBatchPhase::preflight;
  uint8_t flags = capsuleBatchResponseAllowed;
  uint8_t reserved[13] = {};
  uint32_t crc32 = 0;
};

struct StoredCapsuleBatchPlan {
  char id[37] = {};
  char targetId[37] = {};
  char source[kCapsuleBatchPathBytes] = {};
  char target[kCapsuleBatchPathBytes] = {};
  int32_t expectedRevision = -1;
  uint8_t flags = 0;
  uint8_t reserved[7] = {};
  uint32_t crc32 = 0;
};
#pragma pack(pop)

inline uint32_t capsuleBatchJournalCrc(const void *value, size_t length) {
  return capsuleTransactionCrc32(
      reinterpret_cast<const uint8_t *>(value), length);
}

inline void sealCapsuleBatchState(StoredCapsuleBatchState &state) {
  state.magic = kCapsuleBatchJournalMagic;
  state.version = kCapsuleBatchJournalVersion;
  state.bytes = sizeof(state);
  state.crc32 = capsuleBatchJournalCrc(
      &state, offsetof(StoredCapsuleBatchState, crc32));
}

inline bool validCapsuleBatchState(const StoredCapsuleBatchState &state) {
  return state.magic == kCapsuleBatchJournalMagic &&
      state.version == kCapsuleBatchJournalVersion &&
      state.bytes == sizeof(state) && state.total <= 500 &&
      state.cursor <= state.total && state.applied <= state.total &&
      static_cast<uint8_t>(state.phase) >=
          static_cast<uint8_t>(CapsuleBatchPhase::preflight) &&
      static_cast<uint8_t>(state.phase) <=
          static_cast<uint8_t>(CapsuleBatchPhase::cleanup) &&
      state.transactionId[0] != '\0' && state.operation[0] != '\0' &&
      state.crc32 == capsuleBatchJournalCrc(
          &state, offsetof(StoredCapsuleBatchState, crc32));
}

inline void sealCapsuleBatchPlan(StoredCapsuleBatchPlan &plan) {
  plan.crc32 = capsuleBatchJournalCrc(
      &plan, offsetof(StoredCapsuleBatchPlan, crc32));
}

inline bool validCapsuleBatchPlan(const StoredCapsuleBatchPlan &plan) {
  return plan.id[0] != '\0' && plan.source[0] != '\0' &&
      plan.crc32 == capsuleBatchJournalCrc(
          &plan, offsetof(StoredCapsuleBatchPlan, crc32));
}

inline const StoredCapsuleBatchState *newestCapsuleBatchState(
    const StoredCapsuleBatchState &first,
    const StoredCapsuleBatchState &second) {
  const bool firstValid = validCapsuleBatchState(first);
  const bool secondValid = validCapsuleBatchState(second);
  if (!firstValid) return secondValid ? &second : nullptr;
  if (!secondValid) return &first;
  return static_cast<int32_t>(second.generation - first.generation) > 0
      ? &second : &first;
}

inline StoredCapsuleBatchState nextCapsuleBatchCheckpoint(
    const StoredCapsuleBatchState &durable) {
  StoredCapsuleBatchState candidate = durable;
  ++candidate.generation;
  sealCapsuleBatchState(candidate);
  return candidate;
}

inline bool acceptCapsuleBatchCheckpoint(
    StoredCapsuleBatchState &durable,
    const StoredCapsuleBatchState &candidate,
    const StoredCapsuleBatchState &readBack) {
  if (!validCapsuleBatchState(candidate) ||
      memcmp(&candidate, &readBack, sizeof(candidate)) != 0) return false;
  durable = candidate;
  return true;
}

// Converts an interrupted checkpoint into an idempotent recovery cursor.  An
// apply whose post-checkpoint is missing is conservatively included in the
// rollback prefix; per-item rollback must tolerate both old and new state.  A
// rollback whose post-checkpoint is missing simply retries the same item.
inline void prepareCapsuleBatchRecovery(StoredCapsuleBatchState &state) {
  if (!validCapsuleBatchState(state)) return;
  state.flags &= ~capsuleBatchResponseAllowed;
  if ((state.flags & capsuleBatchItemInFlight) == 0) return;
  state.flags &= ~capsuleBatchItemInFlight;
  state.flags &= ~capsuleBatchSuccess;
  if (state.phase == CapsuleBatchPhase::apply) {
    const uint16_t uncertain = state.cursor < state.total
        ? static_cast<uint16_t>(state.cursor + 1) : state.total;
    if (state.applied < uncertain) state.applied = uncertain;
    state.cursor = state.applied;
    state.phase = CapsuleBatchPhase::rollback;
  }
  sealCapsuleBatchState(state);
}

}  // namespace pokepod
