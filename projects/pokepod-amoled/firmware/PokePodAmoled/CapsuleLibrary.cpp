#include "CapsuleLibrary.h"

#include <cJSON.h>
#include <algorithm>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <esp_system.h>
#include <utility>

#include "CapsuleCompatibilityPolicy.h"
#include "CapsuleMetadataCodec.h"
#include "CapsulePolicy.h"

namespace pokepod {
namespace {

constexpr size_t kMaxMetadataBytes = 8192;
constexpr size_t kPreviewBytes = 360;

CapsuleStatus parseStatus(const char *value) {
  if (value == nullptr) return CapsuleStatus::damaged;
  if (strcmp(value, "recording") == 0) return CapsuleStatus::recording;
  if (strcmp(value, "recorded") == 0 || strcmp(value, "queued") == 0) {
    return CapsuleStatus::queued;
  }
  if (strcmp(value, "transcribing") == 0) return CapsuleStatus::transcribing;
  if (strcmp(value, "raw_ready") == 0) return CapsuleStatus::rawReady;
  if (strcmp(value, "correcting") == 0) return CapsuleStatus::correcting;
  if (strcmp(value, "ready") == 0) return CapsuleStatus::ready;
  if (strcmp(value, "failed") == 0) return CapsuleStatus::failed;
  return CapsuleStatus::damaged;
}

const char *jsonString(cJSON *root, const char *name) {
  cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
  return cJSON_IsString(item) && item->valuestring != nullptr
      ? item->valuestring : nullptr;
}

void replaceStringOrNull(cJSON *root, const char *name, const String &value) {
  cJSON *replacement = value.isEmpty() ? cJSON_CreateNull()
                                        : cJSON_CreateString(value.c_str());
  cJSON_ReplaceItemInObjectCaseSensitive(root, name, replacement);
}

template <size_t Capacity>
void copyFixed(char (&destination)[Capacity], const String &source) {
  if (Capacity == 0) return;
  const size_t count = source.length() < Capacity - 1
      ? source.length() : Capacity - 1;
  if (count > 0) memcpy(destination, source.c_str(), count);
  destination[count] = '\0';
}

}  // namespace

CapsuleLibrary::~CapsuleLibrary() {
  if (locators_ != nullptr) heap_caps_free(locators_);
  if (scanLocators_ != nullptr) heap_caps_free(scanLocators_);
}

bool CapsuleLibrary::allocateIndex() {
  if (locators_ == nullptr) {
    locators_ = static_cast<CapsuleLocator *>(heap_caps_calloc(
        kCapsuleLocatorCapacity, sizeof(CapsuleLocator),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  }
  if (scanLocators_ == nullptr) {
    scanLocators_ = static_cast<CapsuleLocator *>(heap_caps_calloc(
        kCapsuleLocatorCapacity, sizeof(CapsuleLocator),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  }
  if (locators_ == nullptr || scanLocators_ == nullptr) {
    if (log_ != nullptr) {
      log_->printf("{\"event\":\"capsule_index_allocation_failed\","
                   "\"bytes\":%u}\n",
                   static_cast<unsigned>(kCapsuleLocatorCapacity *
                                         sizeof(CapsuleLocator)));
    }
    return false;
  }
  order_.reserve(kCapsuleLocatorCapacity);
  visible_.reserve(kCapsuleLocatorCapacity);
  return true;
}

bool CapsuleLibrary::begin(fs::FS &fs, Print &log) {
  fs_ = &fs;
  log_ = &log;
  if (!transaction_.begin(fs, log) || !transaction_.recoverAll()) return false;
  if (!allocateIndex()) return false;
  if (!scan()) return false;

  std::vector<String> interrupted;
  for (size_t index = 0; index < locatorCount_; ++index) {
    const CapsuleLocator &locator = locators_[index];
    const CapsuleStatus status = static_cast<CapsuleStatus>(locator.status);
    if (!capsuleLocatorHasFlag(locator, locatorReadOnly) &&
        capsuleStatusNeedsStartupRequeue(statusName(status))) {
      interrupted.push_back(locator.id);
    }
  }
  for (const String &id : interrupted) {
    if (log_ != nullptr) {
      log_->printf("{\"event\":\"asr_startup_requeue\",\"capsule_id\":\"%s\"}\n",
                   id.c_str());
    }
    if (!requeue(id)) return false;
  }
  return true;
}

bool CapsuleLibrary::scan() {
  // The only product caller is begin(): boot must publish an initial index
  // before any consumer can safely query it. Runtime paths use requestScan()
  // and pollScan().
  if (!startScanWithOwner({}, StorageOwner::capsuleTransaction)) return false;
  while (scanStepper_.active()) stepScan();
  return scanStepper_.state() == CapsuleScanState::completed;
}

bool CapsuleLibrary::requestScan(const CapsuleScanBudget &budget) {
  if (fs_ == nullptr || locators_ == nullptr || scanLocators_ == nullptr) {
    return false;
  }
  CapsuleScanBudget normalized = budget;
  if (normalized.directoryEntries == 0) normalized.directoryEntries = 1;
  if (normalized.readBytes == 0) normalized.readBytes = 512;
  if (!scanRequested_) {
    requestedScanBudget_ = normalized;
  } else {
    // Multiple requesters share one sticky generation. Use the tighter
    // budget so merging requests can never make a loop turn more expensive.
    if (normalized.directoryEntries < requestedScanBudget_.directoryEntries) {
      requestedScanBudget_.directoryEntries = normalized.directoryEntries;
    }
    if (normalized.readBytes < requestedScanBudget_.readBytes) {
      requestedScanBudget_.readBytes = normalized.readBytes;
    }
  }
  scanRequested_ = true;
  return true;
}

CapsuleScanState CapsuleLibrary::pollScan() {
  // One poll performs one action: either advance one bounded slice or attempt
  // to start one requested generation. A generation completed in this call is
  // never followed immediately by another start.
  if (scanStepper_.active()) return stepScan();
  if (!scanRequested_) return scanStepper_.state();
  if (!startScan(requestedScanBudget_)) return scanStepper_.state();
  scanRequested_ = false;
  return scanStepper_.state();
}

bool CapsuleLibrary::startScan(const CapsuleScanBudget &budget) {
  return startScanWithOwner(budget, StorageOwner::capsuleScan);
}

bool CapsuleLibrary::startScanWithOwner(const CapsuleScanBudget &budget,
                                        StorageOwner owner) {
  if (fs_ == nullptr || scanStepper_.active() || !allocateIndex()) return false;
  scanReservation_ = StorageCoordinator::instance().reserve(
      owner, StorageAccess::read,
      owner == StorageOwner::capsuleScan ? 0 : 1000);
  if (!scanReservation_) return false;
  scanOwner_ = owner;
  resetScanTransient();
  scanDirectories_.reserve(64);
  scanDirectories_.push_back({kCapsuleInbox, "Inbox", 0, false});
  scanDirectories_.push_back({kCapsuleArchive, "Archive", 0, false});
  scanDirectories_.push_back({kCapsuleTrash, ".trash", 0, false});
  scanDirectories_.push_back({kCapsuleRoot, "", 0, true});
  scanStartedUs_ = esp_timer_get_time();
  maximumScanStepUs_ = 0;
  maximumScanLeaseUs_ = 0;
  scanStepper_.begin(budget);
  return true;
}

CapsuleScanState CapsuleLibrary::stepScan() {
  if (!scanStepper_.active()) return scanStepper_.state();
  const int64_t stepStartedUs = esp_timer_get_time();
  const auto finishStepTiming = [this, stepStartedUs]() {
    const uint32_t elapsed = static_cast<uint32_t>(
        esp_timer_get_time() - stepStartedUs);
    if (elapsed > maximumScanStepUs_) maximumScanStepUs_ = elapsed;
  };
  if (scanCancelRequested_ || scanFailureRequested_) {
    if (!scanStepper_.startSlice()) return scanStepper_.state();
    const int64_t leaseStartedUs = esp_timer_get_time();
    StorageIoLease scanIo = StorageCoordinator::instance().acquireIo(
        scanOwner_, StorageAccess::read, 0);
    if (!scanIo) {
      scanStepper_.finishSlice();
      finishStepTiming();
      return scanStepper_.state();
    }
    if (scanDirectory_) scanDirectory_.close();
    if (scanPending_.file) scanPending_.file.close();
    scanDirectoryOpen_ = false;
    scanIo.release();
    const uint32_t leaseElapsed = static_cast<uint32_t>(
        esp_timer_get_time() - leaseStartedUs);
    if (leaseElapsed > maximumScanLeaseUs_) {
      maximumScanLeaseUs_ = leaseElapsed;
    }
    scanStepper_.finishSlice();
    finishScan(scanCancelRequested_ ? CapsuleScanState::cancelled
                                    : CapsuleScanState::failed);
    finishStepTiming();
    return scanStepper_.state();
  }
  if (!scanStepper_.startSlice()) return scanStepper_.state();

  bool recordReady = false;
  bool traversalComplete = false;
  const int64_t leaseStartedUs = esp_timer_get_time();
  StorageIoLease scanIo = StorageCoordinator::instance().acquireIo(
      scanOwner_, StorageAccess::read, 0);
  if (!scanIo) {
    scanStepper_.finishSlice();
    finishStepTiming();
    return scanStepper_.state();
  }
  if (scanPending_.phase != ScanMetadataPhase::none) {
    recordReady = readPendingMetadataSlice();
    if (recordReady) {
      scanPending_.hasWav = fs_->exists(
          scanPending_.directory + "/audio.wav");
      scanPending_.hasM4a = fs_->exists(
          scanPending_.directory + "/audio.m4a");
    }
  } else {
    traversalComplete = processDirectorySlice();
  }
  scanIo.release();
  const uint32_t leaseElapsed = static_cast<uint32_t>(
      esp_timer_get_time() - leaseStartedUs);
  if (leaseElapsed > maximumScanLeaseUs_) {
    maximumScanLeaseUs_ = leaseElapsed;
  }
  scanStepper_.finishSlice();

  if (recordReady) {
    CapsuleSummary record;
    const bool damaged = scanPending_.damaged ||
        !readRecordText(scanPending_.directory, scanPending_.folder,
                        scanPending_.capsuleText,
                        scanPending_.processingText, record, false);
    if (damaged) {
      populateDamagedRecord(scanPending_.directory, scanPending_.folder,
                            scanPending_.id, record, false);
    }
    if (record.status == CapsuleStatus::damaged) {
      if (scanPending_.hasWav) {
        record.audioFile = "audio.wav";
        record.audioFormat = "wav-pcm-s16le";
      } else if (scanPending_.hasM4a) {
        record.audioFile = "audio.m4a";
        record.audioFormat = "m4a-aac-lc";
      }
    }
    if (!appendStagedRecord(record)) scanIndexOverflow_ = true;
    scanPending_ = ScanPendingRecord();
  }
  if (scanIndexOverflow_) {
    // Defer terminal cleanup to one final bounded slice so persistent
    // directory handles are closed under the same physical IO lease.
    scanFailureRequested_ = true;
  } else if (traversalComplete &&
             scanPending_.phase == ScanMetadataPhase::none) {
    finishScan(CapsuleScanState::completed);
  }
  finishStepTiming();
  return scanStepper_.state();
}

void CapsuleLibrary::cancelScan() {
  if (scanStepper_.active()) scanCancelRequested_ = true;
}

bool CapsuleLibrary::processDirectorySlice() {
  while (scanDirectoryIndex_ < scanDirectories_.size()) {
    const ScanDirectoryTask task = scanDirectories_[scanDirectoryIndex_];
    if (!scanDirectoryOpen_) {
      if (scanStepper_.remainingDirectoryEntries() == 0) return false;
      scanStepper_.consumeDirectoryEntry();
      scanDirectory_ = fs_->open(task.path);
      scanDirectoryOpen_ = scanDirectory_ && scanDirectory_.isDirectory();
      if (!scanDirectoryOpen_) {
        if (scanDirectory_) scanDirectory_.close();
        ++scanDirectoryIndex_;
      }
      return false;
    }
    if (scanStepper_.remainingDirectoryEntries() == 0) return false;
    scanStepper_.consumeDirectoryEntry();
    File entry = scanDirectory_.openNextFile();
    if (!entry) {
      scanDirectory_.close();
      scanDirectoryOpen_ = false;
      ++scanDirectoryIndex_;
      return false;
    }
    const String fullName = entry.name();
    const bool isDirectory = entry.isDirectory();
    entry.close();
    const int slash = fullName.lastIndexOf('/');
    const String name = slash >= 0 ? fullName.substring(slash + 1) : fullName;
    if (!isDirectory) return false;
    if (task.discoverRoot) {
      if (!name.startsWith(".") && name != "Inbox" && name != "Archive") {
        scanDirectories_.push_back(
            {String(kCapsuleRoot) + "/" + name, name, 1, false});
      }
    } else if (isUuid(name.c_str())) {
      scanPending_.directory = task.path + "/" + name;
      scanPending_.folder = task.folder;
      scanPending_.id = name;
      scanPending_.phase = ScanMetadataPhase::capsule;
    } else if (task.depth < 2 && !name.startsWith(".")) {
      scanDirectories_.push_back(
          {task.path + "/" + name, task.folder + "/" + name,
           static_cast<uint8_t>(task.depth + 1), false});
    }
    return false;
  }
  return true;
}

bool CapsuleLibrary::openPendingMetadata(const char *name) {
  scanPending_.file = fs_->open(scanPending_.directory + "/" + name,
                                FILE_READ);
  if (!scanPending_.file || scanPending_.file.isDirectory() ||
      scanPending_.file.size() == 0 ||
      scanPending_.file.size() > kMaxMetadataBytes) {
    if (scanPending_.file) scanPending_.file.close();
    scanPending_.damaged = true;
    scanPending_.phase = ScanMetadataPhase::ready;
    return false;
  }
  return true;
}

bool CapsuleLibrary::readPendingMetadataSlice() {
  if (scanPending_.phase == ScanMetadataPhase::ready) return true;
  String *target = scanPending_.phase == ScanMetadataPhase::capsule
      ? &scanPending_.capsuleText : &scanPending_.processingText;
  const char *name = scanPending_.phase == ScanMetadataPhase::capsule
      ? "capsule.json" : "processing.json";
  if (!scanPending_.file && !openPendingMetadata(name)) return true;

  uint8_t chunk[512];
  while (scanPending_.file.available() &&
         scanStepper_.remainingReadBytes() > 0) {
    const size_t request = scanStepper_.remainingReadBytes() < sizeof(chunk)
        ? scanStepper_.remainingReadBytes() : sizeof(chunk);
    const int count = scanPending_.file.read(chunk, request);
    if (count <= 0) {
      scanPending_.damaged = true;
      scanPending_.file.close();
      scanPending_.phase = ScanMetadataPhase::ready;
      return true;
    }
    const size_t accepted = scanStepper_.consumeReadBytes(
        static_cast<size_t>(count));
    target->concat(reinterpret_cast<const char *>(chunk), accepted);
    if (target->length() > kMaxMetadataBytes) {
      scanPending_.damaged = true;
      scanPending_.file.close();
      scanPending_.phase = ScanMetadataPhase::ready;
      return true;
    }
  }
  if (scanPending_.file.available()) return false;
  scanPending_.file.close();
  target->trim();
  if (target->isEmpty()) {
    scanPending_.damaged = true;
    scanPending_.phase = ScanMetadataPhase::ready;
    return true;
  }
  if (scanPending_.phase == ScanMetadataPhase::capsule) {
    scanPending_.phase = ScanMetadataPhase::processing;
    return false;
  }
  scanPending_.phase = ScanMetadataPhase::ready;
  return true;
}

bool CapsuleLibrary::appendStagedRecord(const CapsuleSummary &record) {
  if (scanLocatorCount_ >= kCapsuleLocatorCapacity) return false;
  copyToLocator(record, scanLocators_[scanLocatorCount_++]);
  return true;
}

void CapsuleLibrary::resetScanTransient() {
  // Every terminal path closes open handles while holding its short IO lease.
  scanDirectories_.clear();
  scanDirectoryIndex_ = 0;
  scanDirectoryOpen_ = false;
  scanPending_ = ScanPendingRecord();
  scanLocatorCount_ = 0;
  scanCancelRequested_ = false;
  scanFailureRequested_ = false;
  scanIndexOverflow_ = false;
}

void CapsuleLibrary::finishScan(CapsuleScanState state) {
  // Filesystem handles have already been closed inside the current slice.
  scanDirectoryOpen_ = false;
  scanPending_ = ScanPendingRecord();
  scanReservation_.release();

  if (state == CapsuleScanState::completed) {
    std::swap(locators_, scanLocators_);
    locatorCount_ = scanLocatorCount_;
    indexOverflow_ = false;
    invalidateRecordCache();
    requestPublish();
    ++fullScanCount_;
    const uint32_t elapsedUs = static_cast<uint32_t>(
        esp_timer_get_time() - scanStartedUs_);
    lastScanUs_ = elapsedUs;
    if (elapsedUs > maxScanUs_) maxScanUs_ = elapsedUs;
    scanStepper_.complete();
    if (log_ != nullptr) {
      log_->printf("{\"event\":\"capsule_scan\",\"count\":%u,"
                   "\"pending\":%u,\"slices\":%u}\n",
                   static_cast<unsigned>(locatorCount_),
                   static_cast<unsigned>(pendingCount()),
                   static_cast<unsigned>(scanStepper_.slices()));
    }
  } else if (state == CapsuleScanState::cancelled) {
    scanStepper_.cancel();
  } else {
    scanStepper_.fail();
    indexOverflow_ = scanIndexOverflow_;
    if (scanIndexOverflow_ && log_ != nullptr) {
      log_->printf("{\"event\":\"capsule_index_capacity_exceeded\","
                   "\"capacity\":%u}\n",
                   static_cast<unsigned>(kCapsuleLocatorCapacity));
    }
  }
  scanDirectories_.clear();
  scanLocatorCount_ = 0;
}

bool CapsuleLibrary::includeInboxCapsule(const String &id) {
  if (!isUuid(id.c_str())) return false;
  return refreshRecord(id, String(kCapsuleInbox) + "/" + id, "Inbox");
}

bool CapsuleLibrary::readRecord(const String &directory, const String &folder,
                                CapsuleSummary &record) const {
  return readRecordText(
      directory, folder,
      readText(directory + "/capsule.json", kMaxMetadataBytes),
      readText(directory + "/processing.json", kMaxMetadataBytes), record);
}

bool CapsuleLibrary::readRecordText(const String &directory,
                                    const String &folder,
                                    const String &capsuleText,
                                    const String &processingText,
                                    CapsuleSummary &record,
                                    bool detectDamagedAudio) const {
  const int slash = directory.lastIndexOf('/');
  const String directoryId = slash >= 0
      ? directory.substring(slash + 1) : directory;
  if (!isUuid(directoryId.c_str())) return false;
  CapsuleDecodedMetadata decoded;
  if (!CapsuleMetadataCodec::decode(directoryId, capsuleText, processingText,
                                    decoded)) {
    populateDamagedRecord(directory, folder, directoryId, record,
                          detectDamagedAudio);
    return true;
  }
  record = CapsuleSummary();
  record.id = decoded.id;
  record.directory = directory;
  record.folder = folder;
  record.title = decoded.title;
  record.createdAt = decoded.createdAt;
  record.updatedAt = decoded.updatedAt;
  record.audioFile = decoded.audioFile;
  record.audioFormat = decoded.audioFormat;
  record.errorStage = decoded.errorStage;
  record.error = decoded.error;
  record.status = parseStatus(decoded.status.c_str());
  record.favorite = decoded.favorite;
  record.archived = folder == "Archive" || folder.startsWith("Archive/");
  record.trashed = folder == ".trash" || folder.startsWith(".trash/");
  record.readOnly = decoded.readOnly;
  record.capsuleSchemaVersion = decoded.capsuleSchemaVersion;
  record.processingSchemaVersion = decoded.processingSchemaVersion;
  record.revision = decoded.revision;
  record.processingRevision = decoded.processingRevision;
  record.durationMs = decoded.durationMs;
  record.sampleRateHz = decoded.sampleRateHz;
  record.channels = decoded.channels;
  record.bitsPerSample = decoded.bitsPerSample;
  return true;
}

void CapsuleLibrary::populateDamagedRecord(
    const String &directory, const String &folder, const String &directoryId,
    CapsuleSummary &record, bool detectAudio) const {
  record = CapsuleSummary();
  record.id = directoryId;
  record.directory = directory;
  record.folder = folder;
  record.title = "胶囊需要检查";
  record.status = CapsuleStatus::damaged;
  record.readOnly = true;
  record.archived = folder == "Archive" || folder.startsWith("Archive/");
  record.trashed = folder == ".trash" || folder.startsWith(".trash/");
  if (detectAudio && fs_ != nullptr && fs_->exists(directory + "/audio.wav")) {
    record.audioFile = "audio.wav";
    record.audioFormat = "wav-pcm-s16le";
  } else if (detectAudio && fs_ != nullptr &&
             fs_->exists(directory + "/audio.m4a")) {
    record.audioFile = "audio.m4a";
    record.audioFormat = "m4a-aac-lc";
  }
  record.errorStage = "metadata";
  record.error = "胶囊需要检查";
}

size_t CapsuleLibrary::pendingCount() const {
  size_t count = 0;
  for (size_t index = 0; index < locatorCount_; ++index) {
    const CapsuleLocator &locator = locators_[index];
    if (!capsuleLocatorHasFlag(locator, locatorReadOnly) &&
        !capsuleLocatorHasFlag(locator, locatorArchived) &&
        !capsuleLocatorHasFlag(locator, locatorTrashed) &&
        capsuleLocatorHasFlag(locator, locatorPending)) ++count;
  }
  return count;
}

const CapsuleSummary *CapsuleLibrary::at(size_t index,
                                         bool loadPreview) const {
  return index < visible_.size()
      ? cachedRecord(visible_[index], loadPreview) : nullptr;
}

const CapsuleSummary *CapsuleLibrary::nextQueued() const {
  for (const size_t index : order_) {
    const CapsuleLocator &locator = locators_[index];
    if (!capsuleLocatorHasFlag(locator, locatorReadOnly) &&
        !capsuleLocatorHasFlag(locator, locatorArchived) &&
        !capsuleLocatorHasFlag(locator, locatorTrashed) &&
        static_cast<CapsuleStatus>(locator.status) == CapsuleStatus::queued) {
      return cachedRecord(index, false);
    }
  }
  return nullptr;
}

const CapsuleSummary *CapsuleLibrary::find(const String &id) const {
  const size_t index = recordIndex(id);
  return index < locatorCount_ ? cachedRecord(index, false) : nullptr;
}

bool CapsuleLibrary::hydrate(const String &id, CapsuleSummary &record,
                             bool loadPreview) const {
  const size_t index = recordIndex(id);
  if (index >= locatorCount_ || !hydrateLocator(locators_[index], record)) {
    return false;
  }
  if (loadPreview) record.preview = readBestText(record, kPreviewBytes);
  return true;
}

bool CapsuleLibrary::markTranscribing(const String &id) {
  return updateProcessing(id, CapsuleStatus::transcribing, "", "", "", true) &&
      refreshExisting(id);
}

bool CapsuleLibrary::commitRawText(const String &id, const String &text) {
  StorageReservation reservation = StorageCoordinator::instance().reserve(
      StorageOwner::capsuleTransaction, StorageAccess::mutation, 1000);
  if (!reservation) return false;
  const CapsuleSummary *record = find(id);
  if (record == nullptr || record->readOnly || text.isEmpty()) return false;
  const String directory = record->directory;
  String processing;
  if (!prepareProcessing(id, CapsuleStatus::rawReady, "raw.txt", "", "",
                         false, processing)) return false;
  const bool committed = transaction_.commitTextPair(
      id.c_str(), directory + "/raw.txt", text + "\n",
      directory + "/processing.json", processing,
      StorageOwner::capsuleTransaction);
  return committed && refreshExisting(id);
}

bool CapsuleLibrary::markFailure(const String &id, const String &stage,
                                 const String &error) {
  return updateProcessing(id, CapsuleStatus::failed, "", stage, error, false) &&
      refreshExisting(id);
}

bool CapsuleLibrary::markRetryable(const String &id, const String &stage,
                                   const String &error) {
  return updateProcessing(id, CapsuleStatus::queued, "", stage, error, false) &&
      refreshExisting(id);
}

bool CapsuleLibrary::requeue(const String &id) {
  return updateProcessing(id, CapsuleStatus::queued, "", "", "", false) &&
      refreshExisting(id);
}

bool CapsuleLibrary::toggleFavorite(const String &id) {
  const CapsuleSummary *record = find(id);
  return record != nullptr && !record->readOnly &&
      updateFavorite(id, !record->favorite) && refreshExisting(id);
}

bool CapsuleLibrary::archive(const String &id) {
  StorageReservation reservation = StorageCoordinator::instance().reserve(
      StorageOwner::capsuleTransaction, StorageAccess::mutation, 1000);
  if (!reservation) return false;
  const CapsuleSummary *record = find(id);
  if (record == nullptr || record->readOnly || record->archived ||
      record->trashed ||
      record->status == CapsuleStatus::transcribing ||
      !safeArchiveOriginalFolder(record->folder.c_str())) return false;
  cJSON *metadata = cJSON_CreateObject();
  if (metadata == nullptr) return false;
  cJSON_AddNumberToObject(metadata, "schemaVersion", 1);
  cJSON_AddStringToObject(metadata, "capsuleId", id.c_str());
  cJSON_AddStringToObject(metadata, "originalFolder", record->folder.c_str());
  char *encoded = cJSON_Print(metadata);
  const String metadataPath = record->directory + "/" +
      kCapsuleArchiveMetadata;
  const bool wrote = encoded != nullptr &&
      writeTextAtomic(metadataPath, String(encoded) + "\n");
  cJSON_free(encoded);
  cJSON_Delete(metadata);
  if (!wrote) return false;
  const String target = String(kCapsuleArchive) + "/" + id;
  if (fs_->exists(target) || !fs_->rename(record->directory, target)) {
    fs_->remove(metadataPath);
    return false;
  }
  return refreshRecord(id, target, "Archive");
}

bool CapsuleLibrary::unarchive(const String &id) {
  StorageReservation reservation = StorageCoordinator::instance().reserve(
      StorageOwner::capsuleTransaction, StorageAccess::mutation, 1000);
  if (!reservation) return false;
  const CapsuleSummary *record = find(id);
  if (record == nullptr || record->readOnly || !record->archived ||
      record->trashed ||
      record->status == CapsuleStatus::transcribing) return false;
  const String metadataText = readText(
      record->directory + "/" + kCapsuleArchiveMetadata, kMaxMetadataBytes);
  cJSON *metadata = cJSON_ParseWithLength(metadataText.c_str(),
                                          metadataText.length());
  const char *original = metadata == nullptr
      ? nullptr : jsonString(metadata, "originalFolder");
  String targetDirectory = safeArchiveOriginalFolder(original)
      ? safeRestoreDirectory(original) : String(kCapsuleInbox);
  cJSON_Delete(metadata);
  String target = targetDirectory + "/" + id;
  if (fs_->exists(target) && targetDirectory != kCapsuleInbox) {
    targetDirectory = kCapsuleInbox;
    target = targetDirectory + "/" + id;
  }
  if (fs_->exists(target) || !fs_->rename(record->directory, target)) {
    return false;
  }
  fs_->remove(target + "/" + kCapsuleArchiveMetadata);
  String folder = targetDirectory == kCapsuleInbox
      ? String("Inbox")
      : targetDirectory.substring(String(kCapsuleRoot).length() + 1);
  return refreshRecord(id, target, folder);
}

bool CapsuleLibrary::trash(const String &id, const String &trashedAt) {
  StorageReservation reservation = StorageCoordinator::instance().reserve(
      StorageOwner::capsuleTransaction, StorageAccess::mutation, 1000);
  if (!reservation) return false;
  const CapsuleSummary *record = find(id);
  if (record == nullptr || record->readOnly || record->trashed ||
      trashedAt.isEmpty() ||
      record->status == CapsuleStatus::transcribing) return false;
  cJSON *metadata = cJSON_CreateObject();
  if (metadata == nullptr) return false;
  cJSON_AddNumberToObject(metadata, "schemaVersion", 1);
  cJSON_AddStringToObject(metadata, "capsuleId", id.c_str());
  cJSON_AddStringToObject(metadata, "trashedAt", trashedAt.c_str());
  cJSON_AddStringToObject(metadata, "originalFolder", record->folder.c_str());
  cJSON_AddNumberToObject(metadata, "revision", 1);
  char *encoded = cJSON_Print(metadata);
  const bool wrote = encoded != nullptr && writeTextAtomic(
      record->directory + "/trash.json", String(encoded) + "\n");
  cJSON_free(encoded);
  cJSON_Delete(metadata);
  const String target = String(kCapsuleTrash) + "/" + id;
  if (!wrote) return false;
  if (fs_->exists(target) || !fs_->rename(record->directory, target)) {
    fs_->remove(record->directory + "/trash.json");
    return false;
  }
  return refreshRecord(id, target, ".trash");
}

bool CapsuleLibrary::restore(const String &id) {
  StorageReservation reservation = StorageCoordinator::instance().reserve(
      StorageOwner::capsuleTransaction, StorageAccess::mutation, 1000);
  if (!reservation) return false;
  const CapsuleSummary *record = find(id);
  if (record == nullptr || record->readOnly || !record->trashed) return false;
  const String metadataText = readText(record->directory + "/trash.json",
                                       kMaxMetadataBytes);
  cJSON *metadata = cJSON_ParseWithLength(metadataText.c_str(),
                                          metadataText.length());
  const char *original = metadata == nullptr
      ? nullptr : jsonString(metadata, "originalFolder");
  String targetDirectory = safeRestoreDirectory(
      original == nullptr ? String() : String(original));
  cJSON_Delete(metadata);
  String target = targetDirectory + "/" + id;
  if (fs_->exists(target) && targetDirectory != kCapsuleInbox) {
    targetDirectory = kCapsuleInbox;
    target = targetDirectory + "/" + id;
  }
  if (fs_->exists(target) || !fs_->rename(record->directory, target)) {
    return false;
  }
  fs_->remove(target + "/trash.json");
  String folder = targetDirectory == kCapsuleInbox
      ? String("Inbox")
      : targetDirectory.substring(String(kCapsuleRoot).length() + 1);
  return refreshRecord(id, target, folder);
}

CapsuleBatchResult CapsuleLibrary::purge(const std::vector<String> &ids) {
  CapsuleBatchResult result;
  if (ids.empty() || fs_ == nullptr) return result;
  StorageReservation reservation = StorageCoordinator::instance().reserve(
      StorageOwner::capsuleTransaction, StorageAccess::mutation, 1000);
  if (!reservation) return result;
  for (const String &id : ids) {
    const CapsuleSummary *record = find(id);
    if (record == nullptr || record->readOnly || !record->trashed ||
        record->status == CapsuleStatus::transcribing ||
        record->directory != String(kCapsuleTrash) + "/" + record->id) {
      result.failedId = id;
      return result;
    }
  }

  if (!fs_->exists(kCapsuleStaging) && !fs_->mkdir(kCapsuleStaging)) {
    result.failedId = ids.front();
    return result;
  }
  String transaction;
  for (uint8_t attempt = 0; attempt < 4 && transaction.isEmpty(); ++attempt) {
    uint8_t randomBytes[16];
    for (size_t offset = 0; offset < sizeof(randomBytes); offset += 4) {
      const uint32_t random = esp_random();
      memcpy(randomBytes + offset, &random, sizeof(random));
    }
    char uuid[37];
    formatUuidV4(randomBytes, uuid);
    const String candidate = String(kCapsuleStaging) + "/purge-local-" + uuid;
    if (!fs_->exists(candidate) && fs_->mkdir(candidate)) transaction = candidate;
  }
  if (transaction.isEmpty()) {
    result.failedId = ids.front();
    return result;
  }

  std::vector<String> staged;
  staged.reserve(ids.size());
  for (const String &id : ids) {
    const String source = String(kCapsuleTrash) + "/" + id;
    const String target = transaction + "/" + id;
    if (!fs_->rename(source, target)) {
      result.failedId = id;
      CapsuleRollbackCounts rollback;
      for (auto iterator = staged.rbegin(); iterator != staged.rend();
           ++iterator) {
        const bool restored = fs_->rename(transaction + "/" + *iterator,
                                          String(kCapsuleTrash) + "/" + *iterator);
        rollback.record(restored);
        if (!restored && result.rollbackFailedId.isEmpty()) {
          result.rollbackFailedId = *iterator;
        }
      }
      result.rollbackAttempted = rollback.attempted;
      result.rollbackFailed = rollback.failed;
      result.rolledBackFully = rollback.fullyRolledBack();
      result.changed = rollback.failed;
      if (result.rolledBackFully) fs_->rmdir(transaction);
      (void)requestScan();
      return result;
    }
    staged.push_back(id);
  }

  result.ok = true;
  result.changed = staged.size();
  deferredPublish_.begin();
  for (const String &id : staged) removeIndexedRecord(id);
  finishDeferredPublish();
  bool cleanupDeferred = false;
  for (const String &id : staged) {
    if (!removeTree(transaction + "/" + id)) {
      cleanupDeferred = true;
      if (log_ != nullptr) {
        log_->printf(
            "{\"event\":\"local_purge_cleanup_deferred\",\"capsuleId\":\"%s\"}\n",
            id.c_str());
      }
    }
  }
  if (!cleanupDeferred) fs_->rmdir(transaction);
  return result;
}

CapsuleBatchResult CapsuleLibrary::batch(
    const std::vector<String> &ids, CapsuleBatchAction action,
    const String &changedAt) {
  CapsuleBatchResult result;
  if (ids.empty()) return result;
  StorageReservation reservation = StorageCoordinator::instance().reserve(
      StorageOwner::capsuleTransaction, StorageAccess::mutation, 1000);
  if (!reservation) return result;
  bool favoriteTarget = true;
  if (action == CapsuleBatchAction::favorite) {
    favoriteTarget = false;
    for (const String &id : ids) {
      const CapsuleSummary *record = find(id);
      if (record != nullptr && !record->favorite) {
        favoriteTarget = true;
        break;
      }
    }
  }
  for (const String &id : ids) {
    const CapsuleSummary *record = find(id);
    if (record == nullptr || record->readOnly ||
        record->status == CapsuleStatus::transcribing) {
      result.failedId = id;
      return result;
    }
    if (action == CapsuleBatchAction::trashOrRestore &&
        !record->trashed && changedAt.isEmpty()) {
      result.failedId = id;
      return result;
    }
  }

  std::vector<String> changed;
  changed.reserve(ids.size());
  deferredPublish_.begin();
  const auto finish = [this](CapsuleBatchResult value) {
    finishDeferredPublish();
    return value;
  };
  for (const String &id : ids) {
    const CapsuleSummary *record = find(id);
    bool ok = false;
    bool changedThisItem = true;
    if (action == CapsuleBatchAction::favorite) {
      changedThisItem = record != nullptr &&
          record->favorite != favoriteTarget;
      ok = !changedThisItem;
      if (!ok) ok = toggleFavorite(id);
    } else if (action == CapsuleBatchAction::archiveOrRestore) {
      ok = scope_ == CapsuleScope::trash ? restore(id) :
          (scope_ == CapsuleScope::archive ? unarchive(id) : archive(id));
    } else {
      ok = scope_ == CapsuleScope::trash ? restore(id)
                                         : trash(id, changedAt);
    }
    if (!ok) {
      result.failedId = id;
      CapsuleRollbackCounts rollback;
      for (auto iterator = changed.rbegin(); iterator != changed.rend();
           ++iterator) {
        bool rollbackOk = false;
        if (action == CapsuleBatchAction::favorite) {
          rollbackOk = toggleFavorite(*iterator);
        } else if (action == CapsuleBatchAction::archiveOrRestore) {
          if (scope_ == CapsuleScope::trash) {
            rollbackOk = trash(*iterator, changedAt);
          } else if (scope_ == CapsuleScope::archive) {
            rollbackOk = archive(*iterator);
          } else {
            rollbackOk = unarchive(*iterator);
          }
        } else if (scope_ == CapsuleScope::trash) {
          rollbackOk = trash(*iterator, changedAt);
        } else {
          rollbackOk = restore(*iterator);
        }
        rollback.record(rollbackOk);
        if (!rollbackOk && result.rollbackFailedId.isEmpty()) {
          result.rollbackFailedId = *iterator;
        }
      }
      result.rollbackAttempted = rollback.attempted;
      result.rollbackFailed = rollback.failed;
      result.rolledBackFully = rollback.fullyRolledBack();
      result.changed = rollback.failed;
      return finish(result);
    }
    if (changedThisItem) changed.push_back(id);
  }
  result.ok = true;
  result.changed = changed.size();
  return finish(result);
}

void CapsuleLibrary::setScope(CapsuleScope scope) {
  if (scope_ == scope) return;
  scope_ = scope;
  rebuildVisible();
}

size_t CapsuleLibrary::recordIndex(const String &id) const {
  for (size_t index = 0; index < locatorCount_; ++index) {
    if (id.equalsIgnoreCase(locators_[index].id)) return index;
  }
  return locatorCount_;
}

bool CapsuleLibrary::refreshRecord(const String &id, const String &directory,
                                   const String &folder) {
  CapsuleSummary refreshed;
  if (!readRecord(directory, folder, refreshed) ||
      !refreshed.id.equalsIgnoreCase(id)) {
    ++refreshFallbackCount_;
    return requestScan();
  }
  const size_t index = recordIndex(id);
  if (index < locatorCount_) {
    replaceIndexedRecord(index, refreshed);
  } else if (!appendIndexedRecord(refreshed)) {
    ++refreshFallbackCount_;
    return requestScan();
  }
  ++incrementalRefreshCount_;
  requestPublish();
  return true;
}

bool CapsuleLibrary::refreshExisting(const String &id) {
  const size_t index = recordIndex(id);
  if (index >= locatorCount_) {
    ++refreshFallbackCount_;
    return requestScan();
  }
  String directory;
  String folder;
  if (!resolveLocator(locators_[index], directory, folder)) {
    ++refreshFallbackCount_;
    return requestScan();
  }
  return refreshRecord(id, directory, folder);
}

void CapsuleLibrary::removeIndexedRecord(const String &id) {
  const size_t index = recordIndex(id);
  if (index >= locatorCount_) return;
  if (index + 1 < locatorCount_) {
    memmove(locators_ + index, locators_ + index + 1,
            (locatorCount_ - index - 1) * sizeof(CapsuleLocator));
  }
  --locatorCount_;
  memset(locators_ + locatorCount_, 0, sizeof(CapsuleLocator));
  invalidateRecordCache();
  requestPublish();
}

bool CapsuleLibrary::appendIndexedRecord(const CapsuleSummary &record) {
  if (locatorCount_ >= kCapsuleLocatorCapacity) return false;
  copyToLocator(record, locators_[locatorCount_]);
  ++locatorCount_;
  invalidateRecordCache(record.id);
  return true;
}

bool CapsuleLibrary::replaceIndexedRecord(size_t index,
                                          const CapsuleSummary &record) {
  if (index >= locatorCount_) return false;
  copyToLocator(record, locators_[index]);
  invalidateRecordCache(record.id);
  return true;
}

void CapsuleLibrary::copyToLocator(const CapsuleSummary &record,
                                   CapsuleLocator &locator) const {
  locator = CapsuleLocator();
  copyFixed(locator.id, record.id);
  copyFixed(locator.createdAt, record.createdAt);
  locator.directoryHash = capsuleDirectoryHash(record.directory.c_str());
  locator.status = static_cast<uint8_t>(record.status);
  if (record.folder == "Inbox" || record.folder.startsWith("Inbox/")) {
    locator.storageArea = static_cast<uint8_t>(CapsuleStorageArea::inbox);
  } else if (record.archived) {
    locator.storageArea = static_cast<uint8_t>(CapsuleStorageArea::archive);
  } else if (record.trashed) {
    locator.storageArea = static_cast<uint8_t>(CapsuleStorageArea::trash);
  } else {
    locator.storageArea = static_cast<uint8_t>(CapsuleStorageArea::custom);
  }
  if (record.audioFile == "audio.wav") {
    locator.audioKind = static_cast<uint8_t>(CapsuleAudioKind::wav);
  } else if (record.audioFile == "audio.m4a") {
    locator.audioKind = static_cast<uint8_t>(CapsuleAudioKind::m4a);
  } else if (!record.audioFile.isEmpty()) {
    locator.audioKind = static_cast<uint8_t>(CapsuleAudioKind::other);
  }
  if (record.favorite) locator.flags |= locatorFavorite;
  if (record.archived) locator.flags |= locatorArchived;
  if (record.trashed) locator.flags |= locatorTrashed;
  if (record.readOnly) locator.flags |= locatorReadOnly;
  if (record.status == CapsuleStatus::queued ||
      record.status == CapsuleStatus::transcribing) {
    locator.flags |= locatorPending;
  }
  if (record.status == CapsuleStatus::failed) locator.flags |= locatorFailed;
  if (record.status == CapsuleStatus::damaged) locator.flags |= locatorDamaged;
}

bool CapsuleLibrary::findLocatorInFolder(
    const String &path, const String &folder, uint8_t depth,
    const CapsuleLocator &locator, String &directory,
    String &resolvedFolder) const {
  const String direct = path + "/" + locator.id;
  if (capsuleDirectoryHash(direct.c_str()) == locator.directoryHash) {
    File candidate = fs_->open(direct);
    const bool found = candidate && candidate.isDirectory();
    if (candidate) candidate.close();
    if (found) {
      directory = direct;
      resolvedFolder = folder;
      return true;
    }
  }
  File parent = fs_->open(path);
  if (!parent || !parent.isDirectory()) return false;
  File entry = parent.openNextFile();
  while (entry) {
    const String fullName = entry.name();
    const bool isDirectory = entry.isDirectory();
    entry.close();
    const int slash = fullName.lastIndexOf('/');
    const String name = slash >= 0 ? fullName.substring(slash + 1) : fullName;
    if (isDirectory && !isUuid(name.c_str()) && depth < 2 &&
        !name.startsWith(".")) {
      const String nestedFolder = folder.isEmpty() ? name : folder + "/" + name;
      if (findLocatorInFolder(path + "/" + name, nestedFolder, depth + 1,
                              locator, directory, resolvedFolder)) {
        parent.close();
        return true;
      }
    }
    entry = parent.openNextFile();
  }
  parent.close();
  return false;
}

bool CapsuleLibrary::resolveLocator(const CapsuleLocator &locator,
                                    String &directory,
                                    String &folder) const {
  if (fs_ == nullptr || !isUuid(locator.id)) return false;
  StorageIoLease lease = StorageCoordinator::instance().acquireIo(
      StorageOwner::capsuleTransaction, StorageAccess::read, 1000);
  if (!lease) return false;

  const CapsuleStorageArea area =
      static_cast<CapsuleStorageArea>(locator.storageArea);
  if (area == CapsuleStorageArea::inbox) {
    return findLocatorInFolder(kCapsuleInbox, "Inbox", 0, locator,
                               directory, folder);
  }
  if (area == CapsuleStorageArea::archive) {
    return findLocatorInFolder(kCapsuleArchive, "Archive", 0, locator,
                               directory, folder);
  }
  if (area == CapsuleStorageArea::trash) {
    return findLocatorInFolder(kCapsuleTrash, ".trash", 0, locator,
                               directory, folder);
  }
  if (area != CapsuleStorageArea::custom) return false;

  File root = fs_->open(kCapsuleRoot);
  if (!root || !root.isDirectory()) return false;
  File entry = root.openNextFile();
  while (entry) {
    const String fullName = entry.name();
    const bool isDirectory = entry.isDirectory();
    entry.close();
    const int slash = fullName.lastIndexOf('/');
    const String name = slash >= 0 ? fullName.substring(slash + 1) : fullName;
    if (isDirectory && !name.startsWith(".") && name != "Inbox" &&
        name != "Archive" &&
        findLocatorInFolder(String(kCapsuleRoot) + "/" + name, name, 1,
                            locator, directory, folder)) {
      root.close();
      return true;
    }
    entry = root.openNextFile();
  }
  root.close();
  return false;
}

bool CapsuleLibrary::hydrateLocator(const CapsuleLocator &locator,
                                    CapsuleSummary &record) const {
  String directory;
  String folder;
  return resolveLocator(locator, directory, folder) &&
      readRecord(directory, folder, record) &&
      record.id.equalsIgnoreCase(locator.id);
}

const CapsuleSummary *CapsuleLibrary::cachedRecord(
    size_t locatorIndex, bool loadPreview) const {
  if (locatorIndex >= locatorCount_) return nullptr;
  ++detailCacheAge_;
  if (detailCacheAge_ == 0) {
    detailCacheAge_ = 1;
    for (DetailCacheEntry &entry : detailCache_) entry.age = 0;
  }
  DetailCacheEntry *victim = &detailCache_[0];
  for (DetailCacheEntry &entry : detailCache_) {
    if (entry.valid && entry.locatorIndex == locatorIndex &&
        entry.summary.id.equalsIgnoreCase(locators_[locatorIndex].id)) {
      entry.age = detailCacheAge_;
      if (loadPreview && !entry.previewLoaded) {
        entry.summary.preview = readBestText(entry.summary, kPreviewBytes);
        entry.previewLoaded = true;
      }
      return &entry.summary;
    }
    if (!entry.valid || entry.age < victim->age) victim = &entry;
  }
  if (!hydrateLocator(locators_[locatorIndex], victim->summary)) {
    victim->valid = false;
    victim->previewLoaded = false;
    victim->age = 0;
    return nullptr;
  }
  victim->locatorIndex = locatorIndex;
  victim->age = detailCacheAge_;
  victim->valid = true;
  victim->previewLoaded = false;
  if (loadPreview) {
    victim->summary.preview = readBestText(victim->summary, kPreviewBytes);
    victim->previewLoaded = true;
  }
  return &victim->summary;
}

void CapsuleLibrary::invalidateRecordCache(const String &id) {
  for (DetailCacheEntry &entry : detailCache_) {
    if (id.isEmpty() || entry.summary.id.equalsIgnoreCase(id)) {
      entry.valid = false;
      entry.previewLoaded = false;
      entry.age = 0;
    }
  }
}

void CapsuleLibrary::requestPublish() {
  if (deferredPublish_.request()) publishRecords();
}

void CapsuleLibrary::publishRecords() {
  invalidateRecordCache();
  order_.clear();
  for (size_t index = 0; index < locatorCount_; ++index) {
    order_.push_back(index);
  }
  std::sort(order_.begin(), order_.end(), [this](size_t left, size_t right) {
    return capsuleLocatorNewer(locators_[left], locators_[right]);
  });
  rebuildVisible();
}

void CapsuleLibrary::finishDeferredPublish() {
  if (deferredPublish_.finish()) publishRecords();
}

void CapsuleLibrary::rebuildVisible() {
  visible_.clear();
  for (const size_t index : order_) {
    if (capsuleLocatorVisible(locators_[index], scope_)) {
      visible_.push_back(index);
    }
  }
  ++revision_;
  if (revision_ == 0) revision_ = 1;
}

String CapsuleLibrary::safeRestoreDirectory(const String &folder) const {
  if (folder.isEmpty() || folder.startsWith(".") || folder.startsWith("/") ||
      folder.indexOf("\\") >= 0 || folder.indexOf("//") >= 0 ||
      folder == "." || folder == ".." || folder.indexOf("/../") >= 0 ||
      folder.startsWith("../") || folder.endsWith("/..")) {
    return String(kCapsuleInbox);
  }
  const String candidate = String(kCapsuleRoot) + "/" + folder;
  StorageIoLease lease = StorageCoordinator::instance().acquireIo(
      StorageOwner::capsuleTransaction, StorageAccess::read, 1000);
  if (!lease) return String(kCapsuleInbox);
  File directory = fs_->open(candidate);
  const bool valid = directory && directory.isDirectory();
  if (directory) directory.close();
  return valid ? candidate : String(kCapsuleInbox);
}

String CapsuleLibrary::readBestText(const CapsuleSummary &record,
                                    size_t maxBytes) const {
  for (const char *name : {"final.md", "polished.md", "raw.txt"}) {
    const String value = readText(record.directory + "/" + name, maxBytes);
    if (!value.isEmpty()) return value;
  }
  return record.title;
}

String CapsuleLibrary::readText(const String &path, size_t maxBytes) const {
  if (fs_ == nullptr || maxBytes == 0) return String();
  StorageIoLease lease = StorageCoordinator::instance().acquireIo(
      StorageOwner::capsuleTransaction, StorageAccess::read, 1000);
  if (!lease) return String();
  File file = fs_->open(path, FILE_READ);
  if (!file || file.isDirectory()) {
    if (file) file.close();
    return String();
  }
  String value;
  const size_t wanted = file.size() < maxBytes ? file.size() : maxBytes;
  if (!value.reserve(wanted + 1)) {
    file.close();
    return String();
  }
  uint8_t chunk[512];
  while (file.available() && value.length() < wanted) {
    const size_t remaining = wanted - value.length();
    const size_t request = remaining < sizeof(chunk) ? remaining : sizeof(chunk);
    const size_t count = file.read(chunk, request);
    if (count == 0) break;
    value.concat(reinterpret_cast<const char *>(chunk), count);
  }
  file.close();
  value.trim();
  return value;
}

bool CapsuleLibrary::writeTextAtomic(const String &path, const String &value) {
  return transaction_.writeTextAtomic(
      path, value, StorageOwner::capsuleTransaction, "capsule-metadata");
}

bool CapsuleLibrary::removeTree(const String &path) {
  StorageIoLease lease = StorageCoordinator::instance().acquireIo(
      StorageOwner::capsuleTransaction, StorageAccess::mutation, 1000);
  if (!lease) return false;
  File root = fs_->open(path);
  if (!root) return true;
  if (!root.isDirectory()) {
    root.close();
    return fs_->remove(path);
  }
  std::vector<String> children;
  File entry = root.openNextFile();
  while (entry) {
    const String full = entry.name();
    entry.close();
    const int slash = full.lastIndexOf('/');
    const String name = slash >= 0 ? full.substring(slash + 1) : full;
    children.push_back(path + "/" + name);
    entry = root.openNextFile();
  }
  root.close();
  for (const String &child : children) {
    if (!removeTree(child)) return false;
  }
  return fs_->rmdir(path);
}

bool CapsuleLibrary::updateProcessing(const String &id, CapsuleStatus status,
                                      const String &rawTextFile,
                                      const String &errorStage,
                                      const String &error,
                                      bool incrementAttempts) {
  StorageReservation reservation = StorageCoordinator::instance().reserve(
      StorageOwner::capsuleTransaction, StorageAccess::mutation, 1000);
  if (!reservation) return false;
  String encoded;
  if (!prepareProcessing(id, status, rawTextFile, errorStage, error,
                         incrementAttempts, encoded)) return false;
  const CapsuleSummary *record = find(id);
  return record != nullptr && writeTextAtomic(
      record->directory + "/processing.json", encoded);
}

bool CapsuleLibrary::prepareProcessing(const String &id, CapsuleStatus status,
                                       const String &rawTextFile,
                                       const String &errorStage,
                                       const String &error,
                                       bool incrementAttempts,
                                       String &encodedValue) {
  const CapsuleSummary *record = find(id);
  if (record == nullptr || record->readOnly) return false;
  const String source = readText(record->directory + "/processing.json",
                                 kMaxMetadataBytes);
  cJSON *root = cJSON_ParseWithLength(source.c_str(), source.length());
  if (root == nullptr) return false;
  cJSON *revision = cJSON_GetObjectItemCaseSensitive(root, "revision");
  const int nextRevision = cJSON_IsNumber(revision) ? revision->valueint + 1 : 1;
  cJSON_ReplaceItemInObjectCaseSensitive(root, "revision", cJSON_CreateNumber(nextRevision));
  cJSON_ReplaceItemInObjectCaseSensitive(root, "status",
                                         cJSON_CreateString(statusName(status)));
  if (!rawTextFile.isEmpty()) replaceStringOrNull(root, "rawTextFile", rawTextFile);
  replaceStringOrNull(root, "errorStage", errorStage);
  replaceStringOrNull(root, "error", error);
  if (incrementAttempts) {
    cJSON *attempts = cJSON_GetObjectItemCaseSensitive(root, "attempts");
    const int count = cJSON_IsNumber(attempts) ? attempts->valueint + 1 : 1;
    cJSON_ReplaceItemInObjectCaseSensitive(root, "attempts", cJSON_CreateNumber(count));
  }
  char *encoded = cJSON_Print(root);
  const bool ok = encoded != nullptr;
  encodedValue = ok ? String(encoded) + "\n" : String();
  cJSON_free(encoded);
  cJSON_Delete(root);
  return ok;
}

bool CapsuleLibrary::updateFavorite(const String &id, bool favorite) {
  StorageReservation reservation = StorageCoordinator::instance().reserve(
      StorageOwner::capsuleTransaction, StorageAccess::mutation, 1000);
  if (!reservation) return false;
  const CapsuleSummary *record = find(id);
  if (record == nullptr || record->readOnly) return false;
  const String path = record->directory + "/capsule.json";
  const String source = readText(path, kMaxMetadataBytes);
  cJSON *root = cJSON_ParseWithLength(source.c_str(), source.length());
  if (root == nullptr) return false;
  cJSON *revision = cJSON_GetObjectItemCaseSensitive(root, "revision");
  const int nextRevision = cJSON_IsNumber(revision) ? revision->valueint + 1 : 1;
  cJSON_ReplaceItemInObjectCaseSensitive(root, "revision", cJSON_CreateNumber(nextRevision));
  cJSON_ReplaceItemInObjectCaseSensitive(root, "favorite", cJSON_CreateBool(favorite));
  char *encoded = cJSON_Print(root);
  const bool ok = encoded != nullptr && writeTextAtomic(path, String(encoded) + "\n");
  cJSON_free(encoded);
  cJSON_Delete(root);
  return ok;
}

const char *CapsuleLibrary::statusName(CapsuleStatus status) {
  switch (status) {
    case CapsuleStatus::recording: return "recording";
    case CapsuleStatus::queued: return "queued";
    case CapsuleStatus::transcribing: return "transcribing";
    case CapsuleStatus::rawReady: return "raw_ready";
    case CapsuleStatus::correcting: return "correcting";
    case CapsuleStatus::ready: return "ready";
    case CapsuleStatus::failed: return "failed";
    case CapsuleStatus::damaged: return "damaged";
  }
  return "damaged";
}

}  // namespace pokepod
