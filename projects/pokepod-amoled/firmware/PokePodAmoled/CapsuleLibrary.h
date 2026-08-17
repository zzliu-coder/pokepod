#pragma once

#include <Arduino.h>
#include <FS.h>
#include <array>
#include <vector>

#include "CapsuleBrowserState.h"
#include "CapsuleTransaction.h"
#include "CapsuleIndexPolicy.h"
#include "CapsuleOperationService.h"
#include "CapsuleScanStepper.h"
#include "DeferredPublish.h"
#include "StorageCoordinator.h"

namespace pokepod {

enum class CapsuleStatus {
  recording,
  queued,
  transcribing,
  rawReady,
  correcting,
  ready,
  failed,
  damaged,
};

enum class CapsuleLibraryStartupState : uint8_t {
  idle = 0,
  waitingForAuthority,
  recoveringTransactions,
  allocatingIndex,
  startingScan,
  scanning,
  selectingInterrupted,
  openingProcessing,
  readingProcessing,
  closingProcessing,
  preparingRequeue,
  startingRequeueCommit,
  pollingRequeueCommit,
  publishingIndex,
  finishingStartup,
  closingBlockedFile,
  ready,
  blocked,
};

struct CapsuleSummary {
  String id;
  String directory;
  String folder;
  String title;
  String createdAt;
  String updatedAt;
  String preview;
  String audioFile;
  String audioFormat;
  String errorStage;
  String error;
  CapsuleStatus status = CapsuleStatus::damaged;
  bool favorite = false;
  bool archived = false;
  bool trashed = false;
  bool readOnly = false;
  int capsuleSchemaVersion = -1;
  int processingSchemaVersion = -1;
  int revision = -1;
  int processingRevision = -1;
  uint32_t durationMs = 0;
  uint32_t sampleRateHz = 0;
  uint8_t channels = 0;
  uint8_t bitsPerSample = 0;
};

enum class CapsuleBatchAction : uint8_t {
  favorite,
  archiveOrRestore,
  trashOrRestore,
};

struct CapsuleBatchResult {
  bool ok = false;
  size_t changed = 0;
  String failedId;
  size_t rollbackAttempted = 0;
  size_t rollbackFailed = 0;
  String rollbackFailedId;
  bool rolledBackFully = false;
};

class CapsuleLibrary : public CapsuleOperationCatalog {
 public:
  ~CapsuleLibrary();
  CapsuleLibrary() = default;
  CapsuleLibrary(const CapsuleLibrary &) = delete;
  CapsuleLibrary &operator=(const CapsuleLibrary &) = delete;

  bool begin(fs::FS &fs, Print &log,
             bool requeueInterruptedTranscription = true);
  CapsuleLibraryStartupState pollStartup(uint32_t nowMs = 0);
  CapsuleLibraryStartupState startupState() const { return startupState_; }
  bool startupActive() const {
    return startupState_ != CapsuleLibraryStartupState::idle &&
        startupState_ != CapsuleLibraryStartupState::ready &&
        startupState_ != CapsuleLibraryStartupState::blocked;
  }
  bool startupReady() const {
    return startupState_ == CapsuleLibraryStartupState::ready;
  }
  bool startupBlocked() const {
    return startupState_ == CapsuleLibraryStartupState::blocked;
  }
  uint32_t startupPolls() const { return startupPolls_; }
  size_t startupMaximumIoBytes() const { return startupMaximumIoBytes_; }
  size_t startupIsolatedCount() const { return startupIsolatedCount_; }
  const String &startupLastIsolatedId() const {
    return startupLastIsolatedId_;
  }
  const String &startupLastIsolationStage() const {
    return startupLastIsolationStage_;
  }
  const String &startupLastIsolationError() const {
    return startupLastIsolationError_;
  }
  bool scan();
  // Queue a full rebuild without doing filesystem work in the caller. The
  // request is sticky: if a scan is running, or a mutation currently prevents
  // a new read reservation, pollScan() will retry it on a later loop turn.
  bool requestScan(const CapsuleScanBudget &budget = {});
  CapsuleScanState pollScan();
  bool startScan(const CapsuleScanBudget &budget = {});
  CapsuleScanState stepScan();
  void cancelScan();
  bool scanRequested() const { return scanRequested_; }
  bool scanActive() const { return scanStepper_.active(); }
  CapsuleScanState scanState() const { return scanStepper_.state(); }
  uint32_t scanSlices() const { return scanStepper_.slices(); }
  size_t maximumScanEntriesPerSlice() const {
    return scanStepper_.maximumDirectoryEntries();
  }
  size_t maximumScanBytesPerSlice() const {
    return scanStepper_.maximumReadBytes();
  }
  uint32_t maximumScanStepUs() const { return maximumScanStepUs_; }
  uint32_t maximumScanLeaseUs() const { return maximumScanLeaseUs_; }
  bool includeInboxCapsule(const String &id);
  size_t count() const { return startupReady() ? visible_.size() : 0; }
  uint32_t revision() const { return startupReady() ? revision_ : 0; }
  uint32_t fullScanCount() const { return fullScanCount_; }
  uint32_t incrementalRefreshCount() const { return incrementalRefreshCount_; }
  uint32_t refreshFallbackCount() const { return refreshFallbackCount_; }
  uint32_t lastScanUs() const { return lastScanUs_; }
  uint32_t maxScanUs() const { return maxScanUs_; }
  bool indexOverflow() const { return indexOverflow_; }
  size_t indexedCount() const { return startupReady() ? locatorCount_ : 0; }
  size_t customPathBytes() const { return startupReady() ? pathPoolUsed_ : 0; }
  static constexpr size_t customPathCapacity() {
    return kCapsuleCustomPathPoolBytes;
  }
  size_t pendingCount() const;
  const CapsuleSummary *at(size_t index, bool loadPreview = false) const;
  const CapsuleSummary *nextQueued() const;
  const CapsuleSummary *find(const String &id) const;
  bool hydrate(const String &id, CapsuleSummary &record,
               bool loadPreview = false) const;

  bool markTranscribing(const String &id);
  bool commitRawText(const String &id, const String &text);
  bool markFailure(const String &id, const String &stage, const String &error);
  bool markRetryable(const String &id, const String &stage, const String &error);
  bool requeue(const String &id);
  bool toggleFavorite(const String &id);
  bool archive(const String &id);
  bool unarchive(const String &id);
  bool trash(const String &id, const String &trashedAt);
  bool restore(const String &id);
  CapsuleBatchResult purge(const std::vector<String> &ids);
  CapsuleBatchResult batch(const std::vector<String> &ids,
                           CapsuleBatchAction action,
                           const String &changedAt);
  void setScope(CapsuleScope scope);
  CapsuleScope scope() const { return scope_; }
  String readBestText(const CapsuleSummary &record, size_t maxBytes = 16384) const;

  bool operationSnapshot(const char *id,
                         CapsuleOperationSnapshot &snapshot) const override;
  bool operationCommitted(const char *id, const char *target,
                          bool removed) override;
  void operationFinished(const char *packedIds, size_t stride, size_t count,
                         bool committed, bool removed) override;

  static const char *statusName(CapsuleStatus status);

 private:
  friend struct CapsuleLibraryStartupTestAccess;
  struct ScanDirectoryTask {
    String path;
    String folder;
    uint8_t depth = 0;
    bool discoverRoot = false;
  };
  enum class ScanMetadataPhase : uint8_t {
    none = 0,
    capsuleOpen,
    capsuleRead,
    capsuleClose,
    processingOpen,
    processingRead,
    processingClose,
    failedClose,
    audioWav,
    audioM4a,
    ready,
  };
  struct ScanPendingRecord {
    String directory;
    String folder;
    String id;
    String capsuleText;
    String processingText;
    File file;
    ScanMetadataPhase phase = ScanMetadataPhase::none;
    bool damaged = false;
    bool hasWav = false;
    bool hasM4a = false;
  };
  class StartupStringByteSource final : public CapsuleTransactionByteSource {
   public:
    void bind(const String *value) { value_ = value; }
    uint32_t length() const override {
      return value_ == nullptr ? 0U : static_cast<uint32_t>(value_->length());
    }
    size_t readAt(uint32_t offset, uint8_t *destination,
                  size_t maximumBytes) override {
      if (value_ == nullptr || destination == nullptr ||
          offset >= value_->length()) {
        return 0;
      }
      const size_t available = value_->length() - offset;
      const size_t count = available < maximumBytes ? available : maximumBytes;
      memcpy(destination, value_->c_str() + offset, count);
      return count;
    }

   private:
    const String *value_ = nullptr;
  };
  struct StartupIsolationDiagnostic {
    char id[37]{};
    char stage[32]{};
    char error[48]{};
  };
  static constexpr size_t kStartupIsolationDiagnosticCapacity = 16;

  bool readRecordText(const String &directory, const String &folder,
                      const String &capsuleText,
                      const String &processingText,
                      CapsuleSummary &record,
                      bool detectDamagedAudio = true) const;
  bool readPendingMetadataSlice();
  bool processDirectorySlice();
  bool appendStagedRecord(const CapsuleSummary &record);
  bool startScanWithOwner(const CapsuleScanBudget &budget,
                          StorageOwner owner);
  void finishScan(CapsuleScanState state);
  void resetScanTransient();
  bool readRecord(const String &directory, const String &folder,
                  CapsuleSummary &record) const;
  String readText(const String &path, size_t maxBytes) const;
  bool writeTextAtomic(const String &path, const String &value);
  bool updateProcessing(const String &id, CapsuleStatus status,
                        const String &rawTextFile, const String &errorStage,
                        const String &error, bool incrementAttempts);
  bool prepareProcessing(const String &id, CapsuleStatus status,
                         const String &rawTextFile, const String &errorStage,
                         const String &error, bool incrementAttempts,
                         String &encoded);
  bool prepareProcessingText(const String &source, CapsuleStatus status,
                             const String &rawTextFile,
                             const String &errorStage, const String &error,
                             bool incrementAttempts, String &encoded,
                             const char *expectedCapsuleId = nullptr);
  void failStartup(const char *stage);
  void isolateStartupRequeue(const char *stage, const char *error);
  void deferStartupRequeueIsolation(const char *stage, const char *error);
  void clearStartupRequeue();
  bool selectStartupRequeue();
  bool updateStartupRequeuedLocator();
  bool startupCommitCanBeIsolated() const;
  void rememberStartupIsolation(const String &id, const char *stage,
                                const char *error);
  const StartupIsolationDiagnostic *startupIsolationDiagnostic(
      const char *id) const;
  void populateStartupIsolatedRecord(const CapsuleLocator &locator,
                                    const String &directory,
                                    const String &folder,
                                    CapsuleSummary &record) const;
  size_t internalPendingCount() const;
  bool updateFavorite(const String &id, bool favorite);
  bool removeTree(const String &path);
  size_t recordIndex(const String &id) const;
  bool refreshRecord(const String &id, const String &directory,
                     const String &folder);
  bool refreshExisting(const String &id);
  void removeIndexedRecord(const String &id);
  bool appendIndexedRecord(const CapsuleSummary &record);
  bool replaceIndexedRecord(size_t index, const CapsuleSummary &record);
  void populateDamagedRecord(const String &directory, const String &folder,
                             const String &directoryId,
                             CapsuleSummary &record,
                             bool detectAudio = true) const;
  bool copyToLocator(const CapsuleSummary &record, CapsuleLocator &locator,
                     char *pathPool, size_t &pathPoolUsed) const;
  bool copyIndexedLocator(const CapsuleLocator &source,
                          CapsuleLocator &destination, char *pathPool,
                          size_t &pathPoolUsed) const;
  bool rebuildPublishedIndex(const CapsuleSummary *replacement,
                             size_t replaceIndex, bool append,
                             size_t removeIndex);
  bool rebuildPublishedLocator(size_t replaceIndex,
                               const CapsuleLocator &replacement,
                               const String &replacementPath);
  bool storeCustomPath(const String &directory, CapsuleLocator &locator,
                       char *pathPool, size_t &pathPoolUsed) const;
  bool customPath(const CapsuleLocator &locator, String &directory) const;
  bool hydrateLocator(const CapsuleLocator &locator,
                      CapsuleSummary &record) const;
  bool resolveLocator(const CapsuleLocator &locator, String &directory,
                      String &folder) const;
  bool findLocatorInFolder(const String &path, const String &folder,
                           uint8_t depth, const CapsuleLocator &locator,
                           String &directory, String &resolvedFolder) const;
  const CapsuleSummary *cachedRecord(size_t locatorIndex,
                                     bool loadPreview) const;
  void invalidateRecordCache(const String &id = String());
  bool allocateIndex();
  void requestPublish();
  void publishRecords();
  void finishDeferredPublish();
  void rebuildVisible();
  String safeRestoreDirectory(const String &folder) const;

  fs::FS *fs_ = nullptr;
  Print *log_ = nullptr;
  CapsuleTransaction transaction_;
  CapsuleTransactionRunner startupTransactionRunner_;
  StorageReservation startupAuthorityReservation_;
  CapsuleLibraryStartupState startupState_ = CapsuleLibraryStartupState::idle;
  bool startupRequeueInterrupted_ = true;
  bool startupScanGeneration_ = false;
  size_t startupRequeueIndex_ = 0;
  size_t startupRequeueLocatorIndex_ = 0;
  String startupRequeueId_;
  String startupRequeuePath_;
  String startupRequeueText_;
  String startupRequeueEncoded_;
  File startupRequeueFile_;
  StartupStringByteSource startupRequeueSource_;
  CapsuleTransactionInput startupRequeueInput_;
  const char *startupDeferredIsolationStage_ = nullptr;
  const char *startupDeferredIsolationError_ = nullptr;
  size_t startupIsolatedCount_ = 0;
  String startupLastIsolatedId_;
  String startupLastIsolationStage_;
  String startupLastIsolationError_;
  std::array<StartupIsolationDiagnostic,
             kStartupIsolationDiagnosticCapacity> startupIsolationDiagnostics_{};
  size_t startupIsolationDiagnosticCount_ = 0;
  size_t startupIsolationDiagnosticNext_ = 0;
  uint32_t startupPolls_ = 0;
  size_t startupMaximumIoBytes_ = 0;
  CapsuleLocator *locators_ = nullptr;
  CapsuleLocator *scanLocators_ = nullptr;
  char *pathPool_ = nullptr;
  char *scanPathPool_ = nullptr;
  size_t locatorCount_ = 0;
  size_t scanLocatorCount_ = 0;
  size_t pathPoolUsed_ = 0;
  size_t scanPathPoolUsed_ = 0;
  std::vector<size_t> order_;
  std::vector<size_t> visible_;
  struct DetailCacheEntry {
    CapsuleSummary summary;
    size_t locatorIndex = 0;
    uint32_t age = 0;
    bool valid = false;
    bool previewLoaded = false;
  };
  mutable std::array<DetailCacheEntry, kCapsuleDetailCacheCapacity> detailCache_;
  mutable uint32_t detailCacheAge_ = 0;
  CapsuleScope scope_ = CapsuleScope::inbox;
  bool indexOverflow_ = false;
  uint32_t revision_ = 0;
  DeferredPublish deferredPublish_;
  uint32_t fullScanCount_ = 0;
  uint32_t incrementalRefreshCount_ = 0;
  uint32_t refreshFallbackCount_ = 0;
  uint32_t lastScanUs_ = 0;
  uint32_t maxScanUs_ = 0;
  CapsuleScanStepper scanStepper_;
  StorageReservation scanReservation_;
  std::vector<ScanDirectoryTask> scanDirectories_;
  size_t scanDirectoryIndex_ = 0;
  File scanDirectory_;
  File scanEntry_;
  ScanPendingRecord scanPending_;
  bool scanDirectoryOpen_ = false;
  bool scanEntryPending_ = false;
  bool scanDirectoryExhausted_ = false;
  bool scanCancelRequested_ = false;
  bool scanFailureRequested_ = false;
  bool scanIndexOverflow_ = false;
  bool scanPathFailure_ = false;
  bool scanRequested_ = false;
  CapsuleScanBudget requestedScanBudget_{};
  int64_t scanStartedUs_ = 0;
  StorageOwner scanOwner_ = StorageOwner::capsuleScan;
  uint32_t maximumScanStepUs_ = 0;
  uint32_t maximumScanLeaseUs_ = 0;
};

}  // namespace pokepod
