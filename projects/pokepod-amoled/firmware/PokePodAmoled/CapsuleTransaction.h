#pragma once

#include <Arduino.h>
#include <FS.h>

#include "CapsuleTransactionPolicy.h"
#include "StorageCoordinator.h"

namespace pokepod {

class CapsuleTransaction {
 public:
  bool begin(fs::FS &fs, Print &log,
             StorageCoordinator &coordinator = StorageCoordinator::instance());
  bool recoverAll(StorageOwner owner = StorageOwner::recovery);

  bool writeTextAtomic(const String &path, const String &value,
                       StorageOwner owner, const char *key = nullptr);
  bool writeBytesAtomic(const String &path, const uint8_t *value, size_t length,
                        StorageOwner owner, const char *key = nullptr);
  bool commitTextPair(const char *key,
                      const String &firstPath, const String &firstValue,
                      const String &secondPath, const String &secondValue,
                      StorageOwner owner);
  bool commitPreparedFile(const char *key, const String &preparedPath,
                          const String &targetPath, StorageOwner owner);

 private:
  struct InputTarget {
    String path;
    const uint8_t *bytes = nullptr;
    size_t length = 0;
    String preparedPath;
  };

  bool commit(const char *key, const InputTarget *targets, uint8_t count,
              StorageOwner owner);
  bool recoverBase(const String &base, StorageOwner owner);
  bool readJournal(const String &path,
                   StoredCapsuleTransactionJournal &journal,
                   StorageOwner owner) const;
  bool writeJournal(const String &path,
                    StoredCapsuleTransactionJournal &journal,
                    StorageOwner owner);
  bool fileFacts(const String &path, uint32_t expectedLength,
                 uint32_t expectedCrc32, bool &exists, bool &matches,
                 StorageOwner owner) const;
  bool fileCrc(const String &path, uint32_t &length, uint32_t &crc,
               StorageOwner owner) const;
  bool writeFile(const String &path, const uint8_t *bytes, size_t length,
                 StorageOwner owner);
  bool ensureDirectory(const char *path, StorageOwner owner);
  bool cleanupBase(const String &base, uint8_t count, StorageOwner owner,
                   bool removeJournal);
  String transactionBase(const char *key, const InputTarget *targets,
                         uint8_t count) const;
  static String sidePath(const String &base, uint8_t index,
                         const char *suffix);

  fs::FS *fs_ = nullptr;
  Print *log_ = nullptr;
  StorageCoordinator *coordinator_ = nullptr;
};

}  // namespace pokepod
