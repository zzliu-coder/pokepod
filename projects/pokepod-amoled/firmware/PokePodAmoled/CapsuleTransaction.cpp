#include "CapsuleTransaction.h"

#include "CapsulePolicy.h"

namespace pokepod {
namespace {

constexpr char kTransactionDirectory[] = "/PokeCapsule/.system/transactions";

uint32_t hashKey(const char *key, const char *path) {
  uint32_t crc = 0xffffffffU;
  if (key != nullptr) {
    crc = capsuleTransactionCrc32Update(
        crc, reinterpret_cast<const uint8_t *>(key), strlen(key));
  }
  if (path != nullptr) {
    crc = capsuleTransactionCrc32Update(
        crc, reinterpret_cast<const uint8_t *>(path), strlen(path));
  }
  return ~crc;
}

}  // namespace

bool CapsuleTransaction::begin(fs::FS &fs, Print &log,
                               StorageCoordinator &coordinator) {
  fs_ = &fs;
  log_ = &log;
  coordinator_ = &coordinator;
  return ensureDirectory(kCapsuleRoot, StorageOwner::recovery) &&
      ensureDirectory(kCapsuleSystem, StorageOwner::recovery) &&
      ensureDirectory(kTransactionDirectory, StorageOwner::recovery);
}

String CapsuleTransaction::sidePath(const String &base, uint8_t index,
                                    const char *suffix) {
  return base + "." + String(index) + suffix;
}

String CapsuleTransaction::transactionBase(const char *key,
                                            const InputTarget *targets,
                                            uint8_t count) const {
  uint32_t hash = hashKey(key, count > 0 ? targets[0].path.c_str() : nullptr);
  if (count > 1) hash ^= hashKey(nullptr, targets[1].path.c_str());
  char name[24];
  snprintf(name, sizeof(name), "/tx-%08lx", static_cast<unsigned long>(hash));
  return String(kTransactionDirectory) + name;
}

bool CapsuleTransaction::ensureDirectory(const char *path,
                                         StorageOwner owner) {
  if (fs_ == nullptr || coordinator_ == nullptr) return false;
  StorageIoLease lease = coordinator_->acquireIo(owner, StorageAccess::mutation,
                                                  1000);
  return lease && (fs_->exists(path) || fs_->mkdir(path));
}

bool CapsuleTransaction::writeFile(const String &path, const uint8_t *bytes,
                                   size_t length, StorageOwner owner) {
  if (bytes == nullptr && length != 0) return false;
  StorageIoLease lease = coordinator_->acquireIo(owner, StorageAccess::mutation,
                                                  1000);
  if (!lease) return false;
  if (fs_->exists(path) && !fs_->remove(path)) return false;
  File file = fs_->open(path, FILE_WRITE);
  if (!file) return false;
  const size_t written = length == 0 ? 0 : file.write(bytes, length);
  file.flush();
  const bool ok = written == length && file.getWriteError() == 0;
  file.close();
  if (!ok) fs_->remove(path);
  return ok;
}

bool CapsuleTransaction::fileCrc(const String &path, uint32_t &length,
                                 uint32_t &crc, StorageOwner owner) const {
  StorageIoLease lease = coordinator_->acquireIo(owner, StorageAccess::read,
                                                  1000);
  if (!lease) return false;
  File file = fs_->open(path, FILE_READ);
  if (!file || file.isDirectory() || file.size() > UINT32_MAX) {
    if (file) file.close();
    return false;
  }
  length = static_cast<uint32_t>(file.size());
  uint32_t state = 0xffffffffU;
  uint8_t buffer[512];
  uint32_t remaining = length;
  while (remaining > 0) {
    const size_t wanted = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
    const int read = file.read(buffer, wanted);
    if (read <= 0) {
      file.close();
      return false;
    }
    state = capsuleTransactionCrc32Update(
        state, buffer, static_cast<size_t>(read));
    remaining -= static_cast<uint32_t>(read);
  }
  file.close();
  crc = ~state;
  return true;
}

bool CapsuleTransaction::fileFacts(const String &path,
                                   uint32_t expectedLength,
                                   uint32_t expectedCrc32, bool &exists,
                                   bool &matches, StorageOwner owner) const {
  {
    StorageIoLease lease = coordinator_->acquireIo(owner, StorageAccess::read,
                                                    1000);
    if (!lease) return false;
    exists = fs_->exists(path);
  }
  matches = false;
  if (!exists) return true;
  uint32_t length = 0;
  uint32_t crc = 0;
  if (!fileCrc(path, length, crc, owner)) return false;
  matches = length == expectedLength && crc == expectedCrc32;
  return true;
}

bool CapsuleTransaction::writeJournal(
    const String &path, StoredCapsuleTransactionJournal &journal,
    StorageOwner owner) {
  finalizeCapsuleTransactionJournal(journal);
  const String temporary = path + ".tmp";
  if (!writeFile(temporary,
                 reinterpret_cast<const uint8_t *>(&journal),
                 sizeof(journal), owner)) return false;
  StorageIoLease lease = coordinator_->acquireIo(owner, StorageAccess::mutation,
                                                  1000);
  if (!lease) return false;
  if (fs_->exists(path) && !fs_->remove(path)) return false;
  return fs_->rename(temporary, path);
}

bool CapsuleTransaction::readJournal(
    const String &path, StoredCapsuleTransactionJournal &journal,
    StorageOwner owner) const {
  StorageIoLease lease = coordinator_->acquireIo(owner, StorageAccess::read,
                                                  1000);
  if (!lease) return false;
  File file = fs_->open(path, FILE_READ);
  if (!file || file.isDirectory() || file.size() != sizeof(journal) ||
      file.read(reinterpret_cast<uint8_t *>(&journal), sizeof(journal)) !=
          sizeof(journal)) {
    if (file) file.close();
    return false;
  }
  file.close();
  return validateCapsuleTransactionJournal(journal);
}

bool CapsuleTransaction::cleanupBase(const String &base, uint8_t count,
                                     StorageOwner owner,
                                     bool removeJournal) {
  StorageIoLease lease = coordinator_->acquireIo(owner, StorageAccess::mutation,
                                                  1000);
  if (!lease) return false;
  bool ok = true;
  for (uint8_t index = 0; index < count; ++index) {
    const String staged = sidePath(base, index, ".new");
    const String backup = sidePath(base, index, ".bak");
    if (fs_->exists(staged) && !fs_->remove(staged)) ok = false;
    if (fs_->exists(backup) && !fs_->remove(backup)) ok = false;
  }
  const String temporary = base + ".journal.tmp";
  if (fs_->exists(temporary) && !fs_->remove(temporary)) ok = false;
  const String journal = base + ".journal";
  if (removeJournal && fs_->exists(journal) && !fs_->remove(journal)) {
    ok = false;
  }
  return ok;
}

bool CapsuleTransaction::recoverBase(const String &base,
                                     StorageOwner owner) {
  StoredCapsuleTransactionJournal journal{};
  if (!readJournal(base + ".journal", journal, owner)) return false;
  CapsuleTransactionTargetFacts facts[kCapsuleTransactionMaximumTargets]{};
  for (uint8_t index = 0; index < journal.targetCount; ++index) {
    const StoredCapsuleTransactionTarget &target = journal.targets[index];
    bool targetExists = false;
    bool targetMatches = false;
    bool newExists = false;
    bool newMatches = false;
    bool backupExists = false;
    bool ignored = false;
    if (!fileFacts(target.path, target.expectedLength, target.expectedCrc32,
                   targetExists, targetMatches, owner) ||
        !fileFacts(sidePath(base, index, ".new"), target.expectedLength,
                   target.expectedCrc32, newExists, newMatches, owner) ||
        !fileFacts(sidePath(base, index, ".bak"), 0, 0,
                   backupExists, ignored, owner)) return false;
    facts[index] = {target.hadOriginal != 0, targetExists, targetMatches,
                    newExists && newMatches, backupExists};
  }
  const CapsuleTransactionRecovery recovery =
      decideCapsuleTransactionRecovery(facts, journal.targetCount);
  if (recovery == CapsuleTransactionRecovery::ambiguous) {
    if (log_ != nullptr) {
      log_->printf("{\"event\":\"capsule_transaction_ambiguous\",\"journal\":\"%s\"}\n",
                   (base + ".journal").c_str());
    }
    return false;
  }

  StorageIoLease lease = coordinator_->acquireIo(owner, StorageAccess::mutation,
                                                  1000);
  if (!lease) return false;
  if (recovery == CapsuleTransactionRecovery::commitNew) {
    for (uint8_t index = 0; index < journal.targetCount; ++index) {
      if (facts[index].targetMatchesNew) continue;
      const String target = journal.targets[index].path;
      const String backup = sidePath(base, index, ".bak");
      if (facts[index].targetExists) {
        if (!facts[index].hadOriginal ||
            (fs_->exists(backup) && !fs_->remove(backup)) ||
            !fs_->rename(target, backup)) return false;
      }
      if (!fs_->rename(sidePath(base, index, ".new"), target)) return false;
    }
  } else if (recovery == CapsuleTransactionRecovery::rollbackOld) {
    for (uint8_t index = 0; index < journal.targetCount; ++index) {
      const String target = journal.targets[index].path;
      const String backup = sidePath(base, index, ".bak");
      if (facts[index].hadOriginal && facts[index].backupExists) {
        if (fs_->exists(target) && !fs_->remove(target)) return false;
        if (!fs_->rename(backup, target)) return false;
      } else if (!facts[index].hadOriginal && facts[index].targetMatchesNew &&
                 !fs_->remove(target)) {
        return false;
      }
    }
  }
  lease.release();
  return cleanupBase(base, journal.targetCount, owner, true);
}

bool CapsuleTransaction::recoverAll(StorageOwner owner) {
  if (fs_ == nullptr || coordinator_ == nullptr) return false;
  StorageReservation reservation = coordinator_->reserve(
      owner, StorageAccess::mutation, 1000);
  if (!reservation) return false;
  // Recover one journal per pass. A successful pass removes that journal, so
  // the loop handles any finite queue without a heap list or a silent limit.
  // Ambiguous state stops with the journal and side files preserved.
  for (;;) {
    String nextBase;
    {
      StorageIoLease lease = coordinator_->acquireIo(
          owner, StorageAccess::read, 1000);
      if (!lease) return false;
      File root = fs_->open(kTransactionDirectory);
      if (!root || !root.isDirectory()) {
        if (root) root.close();
        return false;
      }
      File entry = root.openNextFile();
      while (entry) {
        String path = entry.name();
        const bool regular = !entry.isDirectory();
        entry.close();
        if (regular && path.endsWith(".journal")) {
          if (!path.startsWith("/")) {
            path = String(kTransactionDirectory) + "/" + path;
          }
          nextBase = path.substring(0, path.length() - 8);
          break;
        }
        entry = root.openNextFile();
      }
      root.close();
    }
    if (nextBase.isEmpty()) return true;
    if (!recoverBase(nextBase, owner)) return false;
  }
}

bool CapsuleTransaction::commit(const char *key, const InputTarget *targets,
                                uint8_t count, StorageOwner owner) {
  if (fs_ == nullptr || coordinator_ == nullptr || targets == nullptr ||
      count == 0 || count > kCapsuleTransactionMaximumTargets) return false;
  StorageReservation reservation = coordinator_->reserve(
      owner, StorageAccess::mutation, 1000);
  if (!reservation) return false;
  const String base = transactionBase(key, targets, count);
  {
    StorageIoLease lease = coordinator_->acquireIo(owner, StorageAccess::read,
                                                    1000);
    if (!lease) return false;
    if (fs_->exists(base + ".journal")) {
      lease.release();
      if (!recoverBase(base, owner)) return false;
    }
  }
  if (!cleanupBase(base, count, owner, false)) return false;

  StoredCapsuleTransactionJournal journal{};
  journal.magic = kCapsuleTransactionMagic;
  journal.version = kCapsuleTransactionVersion;
  journal.targetCount = count;
  for (uint8_t index = 0; index < count; ++index) {
    if (!capsuleTransactionPathValid(targets[index].path.c_str())) return false;
    const String staged = sidePath(base, index, ".new");
    const String factsPath = targets[index].preparedPath.isEmpty()
        ? staged : targets[index].preparedPath;
    if (targets[index].preparedPath.isEmpty() &&
        !writeFile(staged, targets[index].bytes,
                          targets[index].length, owner)) {
      cleanupBase(base, count, owner, false);
      return false;
    }
    uint32_t length = 0;
    uint32_t crc = 0;
    if (!fileCrc(factsPath, length, crc, owner)) {
      cleanupBase(base, count, owner, false);
      return false;
    }
    bool hadOriginal = false;
    {
      StorageIoLease lease = coordinator_->acquireIo(
          owner, StorageAccess::read, 1000);
      if (!lease) return false;
      hadOriginal = fs_->exists(targets[index].path);
    }
    if (!setCapsuleTransactionTarget(journal.targets[index],
                                     targets[index].path.c_str(), length, crc,
                                     hadOriginal)) return false;
  }
  if (!writeJournal(base + ".journal", journal, owner)) {
    cleanupBase(base, count, owner, false);
    return false;
  }
  // A unique producer file is moved only after the journal is durable. Before
  // this point a reset leaves the source untouched; after it, recovery sees
  // either the source or the journaled .new artifact and never needs a second
  // full-size WAV copy.
  for (uint8_t index = 0; index < count; ++index) {
    if (targets[index].preparedPath.isEmpty()) continue;
    StorageIoLease lease = coordinator_->acquireIo(
        owner, StorageAccess::mutation, 1000);
    if (!lease || !fs_->rename(targets[index].preparedPath,
                               sidePath(base, index, ".new"))) {
      if (lease) lease.release();
      recoverBase(base, owner);
      return false;
    }
  }
  if (!recoverBase(base, owner)) return false;
  return true;
}

bool CapsuleTransaction::writeTextAtomic(const String &path,
                                         const String &value,
                                         StorageOwner owner,
                                         const char *key) {
  return writeBytesAtomic(path,
      reinterpret_cast<const uint8_t *>(value.c_str()), value.length(), owner,
      key);
}

bool CapsuleTransaction::writeBytesAtomic(const String &path,
                                          const uint8_t *value,
                                          size_t length, StorageOwner owner,
                                          const char *key) {
  const InputTarget target{path, value, length, String()};
  return commit(key, &target, 1, owner);
}

bool CapsuleTransaction::commitTextPair(
    const char *key, const String &firstPath, const String &firstValue,
    const String &secondPath, const String &secondValue, StorageOwner owner) {
  const InputTarget targets[2] = {
      {firstPath, reinterpret_cast<const uint8_t *>(firstValue.c_str()),
       firstValue.length(), String()},
      {secondPath, reinterpret_cast<const uint8_t *>(secondValue.c_str()),
       secondValue.length(), String()},
  };
  return commit(key, targets, 2, owner);
}

bool CapsuleTransaction::commitPreparedFile(const char *key,
                                            const String &preparedPath,
                                            const String &targetPath,
                                            StorageOwner owner) {
  const InputTarget target{targetPath, nullptr, 0, preparedPath};
  return commit(key, &target, 1, owner);
}

}  // namespace pokepod
