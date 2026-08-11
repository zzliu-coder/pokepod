#pragma once

#include <Arduino.h>
#include <FS.h>
#include <array>
#include <vector>

#include "CapsuleBrowserState.h"
#include "CapsuleTransaction.h"
#include "CapsuleIndexPolicy.h"
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

class CapsuleLibrary {
 public:
  ~CapsuleLibrary();
  CapsuleLibrary() = default;
  CapsuleLibrary(const CapsuleLibrary &) = delete;
  CapsuleLibrary &operator=(const CapsuleLibrary &) = delete;

  bool begin(fs::FS &fs, Print &log);
  bool scan();
  bool startScan(const CapsuleScanBudget &budget = {});
  CapsuleScanState stepScan();
  void cancelScan();
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
  size_t count() const { return visible_.size(); }
  uint32_t revision() const { return revision_; }
  uint32_t fullScanCount() const { return fullScanCount_; }
  uint32_t incrementalRefreshCount() const { return incrementalRefreshCount_; }
  uint32_t refreshFallbackCount() const { return refreshFallbackCount_; }
  uint32_t lastScanUs() const { return lastScanUs_; }
  uint32_t maxScanUs() const { return maxScanUs_; }
  bool indexOverflow() const { return indexOverflow_; }
  size_t indexedCount() const { return locatorCount_; }
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

  static const char *statusName(CapsuleStatus status);

 private:
  struct ScanDirectoryTask {
    String path;
    String folder;
    uint8_t depth = 0;
    bool discoverRoot = false;
  };
  enum class ScanMetadataPhase : uint8_t {
    none = 0,
    capsule,
    processing,
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

  bool readRecordText(const String &directory, const String &folder,
                      const String &capsuleText,
                      const String &processingText,
                      CapsuleSummary &record,
                      bool detectDamagedAudio = true) const;
  bool openPendingMetadata(const char *name);
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
  void copyToLocator(const CapsuleSummary &record, CapsuleLocator &locator) const;
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
  CapsuleLocator *locators_ = nullptr;
  CapsuleLocator *scanLocators_ = nullptr;
  size_t locatorCount_ = 0;
  size_t scanLocatorCount_ = 0;
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
  ScanPendingRecord scanPending_;
  bool scanDirectoryOpen_ = false;
  bool scanCancelRequested_ = false;
  bool scanFailureRequested_ = false;
  bool scanIndexOverflow_ = false;
  int64_t scanStartedUs_ = 0;
  StorageOwner scanOwner_ = StorageOwner::capsuleScan;
  uint32_t maximumScanStepUs_ = 0;
  uint32_t maximumScanLeaseUs_ = 0;
};

}  // namespace pokepod
