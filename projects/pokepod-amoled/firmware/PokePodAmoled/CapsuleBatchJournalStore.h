#pragma once

#include <Arduino.h>
#include <FS.h>

#include "CapsuleBatchJournal.h"
#include "StorageCoordinator.h"

namespace pokepod {

class CapsuleBatchJournalStore {
 public:
  enum class LoadResult : uint8_t { loaded, wouldBlock, invalid };
  bool begin(fs::FS &fs) {
    fs_ = &fs;
    return true;
  }

  bool create(const String &transactionId, const char *operation,
              uint16_t total, StorageOwner owner,
              StoredCapsuleBatchState &state,
              const char *finalizeFolder = nullptr);
  bool load(const String &transactionId, StorageOwner owner,
            StoredCapsuleBatchState &state) const;
  LoadResult loadStatus(const String &transactionId, StorageOwner owner,
                        StoredCapsuleBatchState &state) const;
  bool checkpoint(StoredCapsuleBatchState &state, StorageOwner owner);
  bool writePlan(const StoredCapsuleBatchState &state, uint16_t index,
                 StoredCapsuleBatchPlan &plan, StorageOwner owner);
  bool readPlan(const StoredCapsuleBatchState &state, uint16_t index,
                StoredCapsuleBatchPlan &plan, StorageOwner owner) const;
  bool erase(const String &transactionId, StorageOwner owner);
  bool quarantine(const String &transactionId, const String &suffix,
                  StorageOwner owner);
  String path(const String &transactionId) const;

  static constexpr const char *kDirectory =
      "/PokeCapsule/.system/transactions/capsule-batch";

 private:
  size_t planOffset(uint16_t index) const {
    return sizeof(StoredCapsuleBatchPlan) * static_cast<size_t>(index);
  }
  String statePath(const String &transactionId, size_t slot) const;
  bool writeStateSlot(const String &path,
                      const StoredCapsuleBatchState &state,
                      StorageOwner owner) const;
  bool readStateSlot(const String &path, StoredCapsuleBatchState &state,
                     StorageOwner owner) const;

  fs::FS *fs_ = nullptr;
};

}  // namespace pokepod
