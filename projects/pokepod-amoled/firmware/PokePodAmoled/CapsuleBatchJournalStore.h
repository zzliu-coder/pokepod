#pragma once

#include <Arduino.h>
#include <FS.h>

#include "CapsuleBatchJournal.h"
#include "StorageCoordinator.h"

namespace pokepod {

class CapsuleBatchJournalStore {
 public:
  bool begin(fs::FS &fs) {
    fs_ = &fs;
    return true;
  }

  bool create(const String &transactionId, const char *operation,
              uint16_t total, StorageOwner owner,
              StoredCapsuleBatchState &state);
  bool load(const String &transactionId, StorageOwner owner,
            StoredCapsuleBatchState &state) const;
  bool checkpoint(StoredCapsuleBatchState &state, StorageOwner owner);
  bool writePlan(const StoredCapsuleBatchState &state, uint16_t index,
                 StoredCapsuleBatchPlan &plan, StorageOwner owner);
  bool readPlan(const StoredCapsuleBatchState &state, uint16_t index,
                StoredCapsuleBatchPlan &plan, StorageOwner owner) const;
  bool erase(const String &transactionId, StorageOwner owner);
  String path(const String &transactionId) const;

  static constexpr const char *kDirectory =
      "/PokeCapsule/.transactions/capsule-batch";

 private:
  size_t planOffset(uint16_t index) const {
    return sizeof(StoredCapsuleBatchState) * 2U +
        sizeof(StoredCapsuleBatchPlan) * static_cast<size_t>(index);
  }
  bool writeStateSlot(const String &path, const char *mode, size_t offset,
                      const StoredCapsuleBatchState &state,
                      StorageOwner owner) const;
  bool readStateSlot(const String &path, size_t offset,
                     StoredCapsuleBatchState &state,
                     StorageOwner owner) const;

  fs::FS *fs_ = nullptr;
};

}  // namespace pokepod
