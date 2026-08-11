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
constexpr uint8_t kCapsuleBatchKnownFlags = capsuleBatchSuccess |
    capsuleBatchResponseAllowed | capsuleBatchRollbackFailed |
    capsuleBatchItemInFlight;

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

inline bool capsuleBatchBoundedString(const char *value, size_t capacity);
inline bool capsuleBatchUuid(const char *value);
inline bool capsuleBatchOperation(const char *value);
inline bool capsuleBatchCapsulePath(const char *value, bool allowTrash);

inline bool validCapsuleBatchState(const StoredCapsuleBatchState &state) {
  const uint8_t phase = static_cast<uint8_t>(state.phase);
  const bool cursorInvariant =
      (state.phase == CapsuleBatchPhase::preflight && state.applied == 0) ||
      (state.phase == CapsuleBatchPhase::apply &&
       state.applied <= state.cursor) ||
      (state.phase == CapsuleBatchPhase::rollback &&
       state.cursor <= state.applied) ||
      state.phase == CapsuleBatchPhase::result ||
      state.phase == CapsuleBatchPhase::cleanup;
  return state.magic == kCapsuleBatchJournalMagic &&
      state.version == kCapsuleBatchJournalVersion &&
      state.bytes == sizeof(state) && state.total <= 500 &&
      state.cursor <= state.total && state.applied <= state.total &&
      phase >= static_cast<uint8_t>(CapsuleBatchPhase::preflight) &&
      phase <= static_cast<uint8_t>(CapsuleBatchPhase::cleanup) &&
      cursorInvariant && (state.flags & ~kCapsuleBatchKnownFlags) == 0 &&
      capsuleBatchUuid(state.transactionId) &&
      capsuleBatchOperation(state.operation) &&
      state.crc32 == capsuleBatchJournalCrc(
          &state, offsetof(StoredCapsuleBatchState, crc32));
}

inline void sealCapsuleBatchPlan(StoredCapsuleBatchPlan &plan) {
  plan.crc32 = capsuleBatchJournalCrc(
      &plan, offsetof(StoredCapsuleBatchPlan, crc32));
}

inline bool capsuleBatchBoundedString(const char *value, size_t capacity) {
  return value != nullptr && capacity > 0 && value[0] != '\0' &&
      memchr(value, '\0', capacity) != nullptr;
}

inline bool capsuleBatchUuid(const char *value) {
  if (!capsuleBatchBoundedString(value, 37) || strlen(value) != 36) {
    return false;
  }
  for (size_t index = 0; index < 36; ++index) {
    const char c = value[index];
    if (index == 8 || index == 13 || index == 18 || index == 23) {
      if (c != '-') return false;
    } else if (!((c >= '0' && c <= '9') ||
                 (c >= 'a' && c <= 'f') ||
                 (c >= 'A' && c <= 'F'))) {
      return false;
    }
  }
  return true;
}

inline bool capsuleBatchOperation(const char *value) {
  if (!capsuleBatchBoundedString(value, 24)) return false;
  static constexpr const char *allowed[] = {
      "setFavorite", "addTags", "removeTags", "renameTag", "mergeTag",
      "deleteTag", "moveCapsules", "copyCapsules", "deleteCapsules",
      "restoreCapsules", "purgeCapsules", "deleteFolderToInbox",
  };
  for (const char *operation : allowed) {
    if (strcmp(value, operation) == 0) return true;
  }
  return false;
}

inline bool capsuleBatchCapsulePath(const char *value, bool allowTrash) {
  if (value == nullptr || value[0] == '\0') return false;
  if (!capsuleBatchBoundedString(value, kCapsuleBatchPathBytes) ||
      strncmp(value, "/PokeCapsule/", 13) != 0) return false;
  const char *first = value + 13;
  const char *last = first;
  uint8_t folderSegments = 0;
  for (const char *cursor = first; ; ++cursor) {
    const unsigned char c = static_cast<unsigned char>(*cursor);
    if (c == '\0' || c == '/') {
      const size_t bytes = static_cast<size_t>(cursor - last);
      if (bytes == 0 || (bytes == 1 && last[0] == '.') ||
          (bytes == 2 && last[0] == '.' && last[1] == '.')) return false;
      if (c == '\0') {
        if (!capsuleBatchUuid(last) || folderSegments == 0 ||
            folderSegments > 2) return false;
        const char *firstSlash = strchr(first, '/');
        if (firstSlash == nullptr) return false;
        const size_t firstBytes = static_cast<size_t>(firstSlash - first);
        const bool trash = firstBytes == 6 &&
            strncmp(first, ".trash", 6) == 0;
        return allowTrash || !trash;
      }
      if (folderSegments == 0 && last[0] == '.' &&
          !(bytes == 6 && strncmp(last, ".trash", 6) == 0)) return false;
      ++folderSegments;
      last = cursor + 1;
    } else {
      if (c < 0x20 || c == '\\') return false;
    }
  }
}

inline bool capsuleBatchTrashPath(const char *value) {
  return value != nullptr && strncmp(value, "/PokeCapsule/.trash/", 20) == 0 &&
      capsuleBatchCapsulePath(value, true);
}

inline bool capsuleBatchActivePath(const char *value) {
  return capsuleBatchCapsulePath(value, false);
}

inline bool validCapsuleBatchPlan(const StoredCapsuleBatchPlan &plan,
                                  const char *operation = nullptr) {
  if (!capsuleBatchUuid(plan.id) ||
      (plan.targetId[0] != '\0' && !capsuleBatchUuid(plan.targetId)) ||
      (operation != nullptr && !capsuleBatchOperation(operation)) ||
      plan.crc32 != capsuleBatchJournalCrc(
          &plan, offsetof(StoredCapsuleBatchPlan, crc32))) return false;
  if (operation == nullptr) {
    return capsuleBatchCapsulePath(plan.source, true) &&
        (plan.target[0] == '\0' ||
         capsuleBatchCapsulePath(plan.target, true));
  }
  const bool metadata = strcmp(operation, "setFavorite") == 0 ||
      strcmp(operation, "addTags") == 0 ||
      strcmp(operation, "removeTags") == 0 ||
      strcmp(operation, "renameTag") == 0 ||
      strcmp(operation, "mergeTag") == 0 ||
      strcmp(operation, "deleteTag") == 0;
  if (metadata) {
    return capsuleBatchActivePath(plan.source) && plan.target[0] == '\0';
  }
  if (strcmp(operation, "deleteCapsules") == 0) {
    return capsuleBatchActivePath(plan.source) &&
        capsuleBatchTrashPath(plan.target);
  }
  if (strcmp(operation, "restoreCapsules") == 0) {
    return capsuleBatchTrashPath(plan.source) &&
        capsuleBatchActivePath(plan.target);
  }
  if (strcmp(operation, "purgeCapsules") == 0) {
    return capsuleBatchTrashPath(plan.source) && plan.target[0] == '\0';
  }
  return capsuleBatchActivePath(plan.source) &&
      capsuleBatchActivePath(plan.target);
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
