#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace pokepod {

constexpr uint32_t kCapsuleTransactionMagic = 0x31585443U;  // CTX1.
constexpr uint16_t kCapsuleTransactionVersion = 1;
constexpr uint8_t kCapsuleTransactionMaximumTargets = 2;
// Link accepts a 161-byte relative folder. Root + folder + UUID + basename
// reaches roughly 230 bytes, so keep the internal journal above that existing
// product limit without changing any wire or shared storage schema.
constexpr size_t kCapsuleTransactionPathBytes = 256;

#pragma pack(push, 1)
struct StoredCapsuleTransactionTarget {
  char path[kCapsuleTransactionPathBytes];
  uint32_t expectedLength;
  uint32_t expectedCrc32;
  uint8_t hadOriginal;
  uint8_t reserved[3];
};

struct StoredCapsuleTransactionJournal {
  uint32_t magic;
  uint16_t version;
  uint8_t targetCount;
  uint8_t reserved;
  StoredCapsuleTransactionTarget targets[kCapsuleTransactionMaximumTargets];
  uint32_t crc32;
};
#pragma pack(pop)

static_assert(sizeof(StoredCapsuleTransactionJournal) <= 548,
              "transaction journal must remain a small fixed record");

inline uint32_t capsuleTransactionCrc32Update(uint32_t crc,
                                              const uint8_t *data,
                                              size_t length) {
  if (data == nullptr && length != 0) return crc;
  for (size_t index = 0; index < length; ++index) {
    crc ^= data[index];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1U) ^ (0xedb88320U &
          static_cast<uint32_t>(-static_cast<int32_t>(crc & 1U)));
    }
  }
  return crc;
}

inline uint32_t capsuleTransactionCrc32(const uint8_t *data, size_t length) {
  return ~capsuleTransactionCrc32Update(0xffffffffU, data, length);
}

inline bool capsuleTransactionPathValid(const char *path) {
  if (path == nullptr || path[0] != '/') return false;
  size_t length = 0;
  while (length < kCapsuleTransactionPathBytes && path[length] != '\0') {
    ++length;
  }
  if (length == 0 || length >= kCapsuleTransactionPathBytes) return false;
  for (size_t index = 0; index + 1 < length; ++index) {
    if (path[index] == '/' && path[index + 1] == '/') return false;
  }
  return strstr(path, "/../") == nullptr &&
      !(length >= 3 && strcmp(path + length - 3, "/..") == 0);
}

inline bool capsuleTransactionPathStartsWith(const char *path,
                                             const char *prefix) {
  return path != nullptr && prefix != nullptr &&
      strncmp(path, prefix, strlen(prefix)) == 0;
}

inline bool capsuleTransactionPathEndsWith(const char *path,
                                           const char *suffix) {
  if (path == nullptr || suffix == nullptr) return false;
  const size_t pathLength = strlen(path);
  const size_t suffixLength = strlen(suffix);
  return pathLength >= suffixLength &&
      strcmp(path + pathLength - suffixLength, suffix) == 0;
}

// Durable journals are recovery authority. Targets therefore stay inside a
// product data leaf and can never name the transaction store or its side
// artifacts. The root and one-level protected paths are directories, not
// transaction targets.
inline bool capsuleTransactionTargetPathValid(const char *path) {
  constexpr const char *root = "/PokeCapsule/";
  if (!capsuleTransactionPathValid(path) ||
      !capsuleTransactionPathStartsWith(path, root)) {
    return false;
  }
  constexpr const char *system = "/PokeCapsule/.system/";
  if (capsuleTransactionPathStartsWith(path, system)) {
    const bool font = strcmp(
        path, "/PokeCapsule/.system/fonts/cjk20.a4") == 0;
    constexpr const char *incoming =
        "/PokeCapsule/.system/commands/incoming/";
    constexpr const char *results =
        "/PokeCapsule/.system/commands/results/";
    const char *leaf = nullptr;
    if (capsuleTransactionPathStartsWith(path, incoming)) {
      leaf = path + strlen(incoming);
    } else if (capsuleTransactionPathStartsWith(path, results)) {
      leaf = path + strlen(results);
    }
    const bool command = leaf != nullptr && leaf[0] != '\0' &&
        strchr(leaf, '/') == nullptr &&
        capsuleTransactionPathEndsWith(leaf, ".json");
    if (!font && !command) return false;
  }
  const char *relative = path + strlen(root);
  if (strchr(relative, '/') == nullptr) return false;
  return !capsuleTransactionPathEndsWith(path, ".journal") &&
      !capsuleTransactionPathEndsWith(path, ".journal.tmp") &&
      !capsuleTransactionPathEndsWith(path, ".new") &&
      !capsuleTransactionPathEndsWith(path, ".bak");
}

// Producer files are inputs, never journal-authorized recovery targets.
inline bool capsuleTransactionPreparedPathValid(const char *path,
                                                const char *target = nullptr) {
  if (!capsuleTransactionPathValid(path) ||
      capsuleTransactionPathEndsWith(path, ".journal") ||
      capsuleTransactionPathEndsWith(path, ".journal.tmp") ||
      capsuleTransactionPathEndsWith(path, ".new") ||
      capsuleTransactionPathEndsWith(path, ".bak")) {
    return false;
  }
  if (capsuleTransactionPathStartsWith(path, "/PokeCapsule/.staging/") ||
      capsuleTransactionPathStartsWith(path, "/PokeCapsule/.recording/")) {
    return true;
  }
  if (target == nullptr || !capsuleTransactionTargetPathValid(target) ||
      !capsuleTransactionPathStartsWith(target, "/PokeCapsule/.system/")) {
    return false;
  }
  const size_t targetLength = strlen(target);
  return strlen(path) == targetLength + 5 &&
      strncmp(path, target, targetLength) == 0 &&
      strcmp(path + targetLength, ".part") == 0;
}

inline bool setCapsuleTransactionTarget(
    StoredCapsuleTransactionTarget &target, const char *path,
    uint32_t expectedLength, uint32_t expectedCrc32, bool hadOriginal) {
  memset(&target, 0, sizeof(target));
  if (!capsuleTransactionTargetPathValid(path)) return false;
  const size_t length = strlen(path);
  if (length >= sizeof(target.path)) return false;
  memcpy(target.path, path, length + 1);
  target.expectedLength = expectedLength;
  target.expectedCrc32 = expectedCrc32;
  target.hadOriginal = hadOriginal ? 1 : 0;
  return true;
}

inline void finalizeCapsuleTransactionJournal(
    StoredCapsuleTransactionJournal &journal) {
  journal.crc32 = capsuleTransactionCrc32(
      reinterpret_cast<const uint8_t *>(&journal),
      offsetof(StoredCapsuleTransactionJournal, crc32));
}

inline bool validateCapsuleTransactionJournal(
    const StoredCapsuleTransactionJournal &journal) {
  if (journal.magic != kCapsuleTransactionMagic ||
      journal.version != kCapsuleTransactionVersion ||
      journal.targetCount == 0 ||
      journal.targetCount > kCapsuleTransactionMaximumTargets ||
      journal.crc32 != capsuleTransactionCrc32(
          reinterpret_cast<const uint8_t *>(&journal),
          offsetof(StoredCapsuleTransactionJournal, crc32))) {
    return false;
  }
  for (uint8_t index = 0; index < journal.targetCount; ++index) {
    if (!capsuleTransactionTargetPathValid(journal.targets[index].path) ||
        journal.targets[index].hadOriginal > 1) return false;
  }
  return true;
}

struct CapsuleTransactionTargetFacts {
  bool hadOriginal = false;
  bool targetExists = false;
  bool targetMatchesNew = false;
  bool newMatchesExpected = false;
  bool backupExists = false;
};

enum class CapsuleTransactionRecovery : uint8_t {
  ambiguous = 0,
  commitNew,
  rollbackOld,
  alreadyCommitted,
};

inline CapsuleTransactionRecovery decideCapsuleTransactionRecovery(
    const CapsuleTransactionTargetFacts *targets, size_t count) {
  if (targets == nullptr || count == 0 ||
      count > kCapsuleTransactionMaximumTargets) {
    return CapsuleTransactionRecovery::ambiguous;
  }
  bool allCommitted = true;
  bool newRecoverable = true;
  bool oldRecoverable = true;
  for (size_t index = 0; index < count; ++index) {
    const CapsuleTransactionTargetFacts &target = targets[index];
    allCommitted = allCommitted && target.targetMatchesNew;
    newRecoverable = newRecoverable &&
        (target.targetMatchesNew || target.newMatchesExpected);
    if (target.hadOriginal) {
      oldRecoverable = oldRecoverable &&
          (target.backupExists ||
           (target.targetExists && !target.targetMatchesNew));
    } else if (target.targetExists && !target.targetMatchesNew) {
      oldRecoverable = false;
    }
  }
  if (allCommitted) return CapsuleTransactionRecovery::alreadyCommitted;
  if (newRecoverable) return CapsuleTransactionRecovery::commitNew;
  if (oldRecoverable) return CapsuleTransactionRecovery::rollbackOld;
  return CapsuleTransactionRecovery::ambiguous;
}

}  // namespace pokepod
