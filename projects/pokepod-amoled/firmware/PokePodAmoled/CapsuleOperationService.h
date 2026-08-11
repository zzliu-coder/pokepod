#pragma once

#include <Arduino.h>
#include <FS.h>
#include <stddef.h>
#include <stdint.h>
#include <vector>

#include "CapsuleBatchJournalStore.h"
#include "CapsulePolicy.h"
#include "CapsuleTransaction.h"
#include "LinkCommandExecutor.h"
#include "LinkTreeStepper.h"
#include "StorageCoordinator.h"

namespace pokepod {

enum class CapsuleOperationAction : uint8_t {
  archive,
  unarchive,
  trash,
  restore,
  purge,
};

struct CapsuleOperationSnapshot {
  char id[37] = {};
  char directory[kCapsuleBatchPathBytes] = {};
  char folder[kCapsuleBatchFolderBytes] = {};
  int32_t revision = -1;
  bool archived = false;
  bool trashed = false;
  bool readOnly = false;
  bool transcribing = false;
};

class CapsuleOperationCatalog {
 public:
  virtual ~CapsuleOperationCatalog() = default;
  // Must be an index-only lookup. Implementations may not touch the
  // filesystem from this callback because it runs inside one service poll.
  virtual bool operationSnapshot(const char *id,
                                 CapsuleOperationSnapshot &snapshot) const = 0;
  // Called once per committed item, on separate poll turns, before the batch
  // terminal is published. This callback must update only the in-memory index.
  virtual bool operationCommitted(const char *id, const char *target,
                                  bool removed) = 0;
  // The durable transaction is terminal before this notification. A catalog
  // may incrementally refresh the listed records or queue a bounded scan.
  virtual void operationFinished(const char *packedIds, size_t stride,
                                 size_t count,
                                 bool committed, bool removed) = 0;
};

struct CapsuleOperationOutcome {
  CapsuleOperationAction action = CapsuleOperationAction::archive;
  size_t requested = 0;
  size_t changed = 0;
  bool committed = false;
  bool rollbackFailed = false;
  bool authorityPreserved = false;
  String failedId;
  String diagnostic;
};

// Local-device capsule mutation owner. submit() only copies fixed-size IDs;
// all filesystem work is driven by poll(), one executor item or <=4 KiB/tree
// primitive at a time. Journals live in a directory separate from Link so the
// two recovery owners can never consume each other's authority.
class CapsuleOperationService {
 public:
  ~CapsuleOperationService();
  CapsuleOperationService() = default;
  CapsuleOperationService(const CapsuleOperationService &) = delete;
  CapsuleOperationService &operator=(const CapsuleOperationService &) = delete;

  bool begin(fs::FS &fs, Print &log,
             StorageCoordinator &coordinator = StorageCoordinator::instance());
  void attachCatalog(CapsuleOperationCatalog &catalog) { catalog_ = &catalog; }
  void poll(uint32_t nowMs);

  bool submit(CapsuleOperationAction action,
              const std::vector<String> &ids,
              const String &changedAt = String());
  bool takeOutcome(CapsuleOperationOutcome &outcome);

  bool readyForMutation() const;
  bool mutationCapabilityBlocked() const;
  bool busy() const;
  bool recoveryActive() const;
  bool sleepBlocker() const { return busy() || recoveryActive(); }
  const char *phaseName() const;
  size_t itemCapacity() const { return kPoolCapacity; }
  size_t transactionLimit() const { return kTransactionLimit; }
  size_t fixedPoolBytes() const;
  size_t maximumPollBytes() const;
  const String &diagnostic() const { return diagnostic_; }

  static constexpr size_t kPoolCapacity = 512;
  static constexpr size_t kTransactionLimit = 500;
  static constexpr const char *kJournalDirectory =
      "/PokeCapsule/.system/transactions/local-capsule";

 private:
  enum class Lifecycle : uint8_t {
    uninitialized,
    ensureDirectories,
    transactionRecoveryStart,
    transactionRecovery,
    scanOpen,
    scanNext,
    scanClose,
    recoveryLoad,
    recoveryCheckpoint,
    running,
    ready,
    blocked,
  };
  enum class Pending : uint8_t {
    none,
    preflight,
    metadataRead,
    metadataCommit,
    applyPath,
    rollbackPath,
    cleanup,
  };

  class StringByteSource final : public CapsuleTransactionByteSource {
   public:
    void bind(const String &value) { value_ = &value; }
    uint32_t length() const override {
      return value_ == nullptr ? 0U : static_cast<uint32_t>(value_->length());
    }
    size_t readAt(uint32_t offset, uint8_t *destination,
                  size_t maximumBytes) override;

   private:
    const String *value_ = nullptr;
  };

  bool allocatePool();
  void resetRequest();
  bool copyRequestIds(const std::vector<String> &ids);
  const char *idAt(size_t index) const;
  bool ensureOneDirectory();
  void pollStartup();
  bool startRecovery(const String &transactionId);
  void enterBlocked(const char *diagnostic);
  void startSubmittedOperation();
  void advanceExecutor(uint32_t nowMs);
  void beginWork(const CapsuleBatchExecutor::Work &work);
  void advancePending(uint32_t nowMs);
  void finishWork(bool ok);
  bool buildPreflightPlan(size_t index);
  bool continueMetadataRead();
  bool preparePlanAfterMetadata();
  bool writeCurrentPlan();
  bool startMetadataCommit(bool rollback);
  bool pollMetadataCommit(uint32_t nowMs);
  bool advancePathMutation(bool rollback);
  bool advanceCleanup();
  bool pollPurgeValidation();
  bool beginPurgeCleanup();
  String purgeTransactionDirectory() const;
  String purgeItemPath(const StoredCapsuleBatchPlan &plan) const;
  const char *operationName() const;
  void syncJournalState(bool itemInFlight);
  bool checkpoint(bool itemInFlight);
  void publishOutcome();
  bool pathExists(const String &path, bool &exists,
                  StorageAccess access = StorageAccess::read);
  bool renamePath(const String &source, const String &target);
  bool removePathIfPresent(const String &path);
  static bool permittedPurgeLeaf(const char *leaf);
  static bool extractOriginalFolder(const String &json, String &folder);
  static String jsonEscaped(const String &value);

  fs::FS *fs_ = nullptr;
  Print *log_ = nullptr;
  StorageCoordinator *coordinator_ = nullptr;
  CapsuleOperationCatalog *catalog_ = nullptr;
  CapsuleBatchJournalStore journalStore_;
  CapsuleBatchExecutor executor_;
  CapsuleTransactionRunner transactionRunner_;
  LinkTreeStepper treeStepper_;
  StorageReservation reservation_;
  Lifecycle lifecycle_ = Lifecycle::uninitialized;
  Pending pending_ = Pending::none;
  CapsuleBatchExecutor::Work work_;
  StoredCapsuleBatchState journalState_;
  StoredCapsuleBatchPlan plan_;
  CapsuleOperationSnapshot snapshot_;
  File startupDirectory_;
  File metadataFile_;
  File purgeDirectory_;
  String startupCandidate_;
  String transactionId_;
  String changedAt_;
  String metadataText_;
  String metadataValue_;
  String metadataPath_;
  String desiredFolder_;
  String diagnostic_;
  String failedId_;
  StringByteSource byteSource_;
  CapsuleOperationOutcome outcome_;
  char (*ids_)[37] = nullptr;
  uint16_t *idSlots_ = nullptr;
  size_t itemCount_ = 0;
  CapsuleOperationAction action_ = CapsuleOperationAction::archive;
  uint8_t ensureDirectoryIndex_ = 0;
  uint8_t startupFailures_ = 0;
  uint8_t pendingStep_ = 0;
  uint8_t cleanupStep_ = 0;
  size_t cleanupIndex_ = 0;
  size_t notificationIndex_ = 0;
  bool requestPending_ = false;
  bool outcomePending_ = false;
  bool recoveryMode_ = false;
  bool planReady_ = false;
  bool purgeEntryUnknown_ = false;
  bool observedSourceExists_ = false;
  bool observedTargetExists_ = false;
  size_t maximumMetadataReadBytes_ = 0;
  size_t maximumPollBytes_ = 0;
};

}  // namespace pokepod
