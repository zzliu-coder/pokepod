#include "CapsuleOperationService.h"

#include <stdlib.h>
#include <string.h>

#ifdef ARDUINO
#include <esp_heap_caps.h>
#include <esp_system.h>
#endif

namespace pokepod {
namespace {

constexpr size_t kIdBytes = 37;
constexpr size_t kIdHashSlots = 1024;
constexpr size_t kMaximumOperationMetadataBytes = 8192;
constexpr uint8_t kMaximumPermanentFailures = 3;
constexpr const char *kTransactionRoot =
    "/PokeCapsule/.system/transactions";

uint32_t idHash(const char *value) {
  uint32_t hash = 2166136261U;
  for (const uint8_t *cursor = reinterpret_cast<const uint8_t *>(value);
       *cursor != 0; ++cursor) {
    hash = (hash ^ *cursor) * 16777619U;
  }
  return hash;
}

String operationLeafName(const String &path) {
  const int slash = path.lastIndexOf('/');
  return slash < 0 ? path : path.substring(static_cast<size_t>(slash + 1));
}

String folderFromCapsulePath(const char *path) {
  if (path == nullptr) return String();
  const String value(path);
  const String prefix = String(kCapsuleRoot) + "/";
  const int slash = value.lastIndexOf('/');
  if (!value.startsWith(prefix.c_str()) ||
      slash <= static_cast<int>(prefix.length())) return String();
  return value.substring(prefix.length(), static_cast<size_t>(slash));
}

String uuidForLocalOperation() {
  uint8_t bytes[16]{};
#ifdef ARDUINO
  for (size_t offset = 0; offset < sizeof(bytes); offset += sizeof(uint32_t)) {
    const uint32_t random = esp_random();
    memcpy(bytes + offset, &random, sizeof(random));
  }
#else
  static uint32_t sequence = 0x74b3a2c1U;
  for (size_t offset = 0; offset < sizeof(bytes); ++offset) {
    sequence ^= sequence << 13;
    sequence ^= sequence >> 17;
    sequence ^= sequence << 5;
    bytes[offset] = static_cast<uint8_t>(sequence >> 8U);
  }
#endif
  char uuid[37];
  formatUuidV4(bytes, uuid);
  return String(uuid);
}

bool isInboxTarget(const char *path, const char *id) {
  return path != nullptr && id != nullptr &&
      String(path) == String(kCapsuleInbox) + "/" + id;
}

}  // namespace

size_t CapsuleOperationService::StringByteSource::readAt(
    uint32_t offset, uint8_t *destination, size_t maximumBytes) {
  if (value_ == nullptr || destination == nullptr ||
      offset >= value_->length()) return 0;
  const size_t remaining = value_->length() - offset;
  const size_t count = remaining < maximumBytes ? remaining : maximumBytes;
  memcpy(destination, value_->c_str() + offset, count);
  return count;
}

CapsuleOperationService::~CapsuleOperationService() {
  if (ids_ != nullptr) {
#ifdef ARDUINO
    heap_caps_free(ids_);
#else
    free(ids_);
#endif
  }
  if (idSlots_ != nullptr) {
#ifdef ARDUINO
    heap_caps_free(idSlots_);
#else
    free(idSlots_);
#endif
  }
}

bool CapsuleOperationService::allocatePool() {
  if (ids_ == nullptr) {
#ifdef ARDUINO
    ids_ = static_cast<char (*)[37]>(heap_caps_calloc(
        kPoolCapacity, kIdBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
#else
    ids_ = static_cast<char (*)[37]>(calloc(kPoolCapacity, kIdBytes));
#endif
  }
  if (idSlots_ == nullptr) {
#ifdef ARDUINO
    idSlots_ = static_cast<uint16_t *>(heap_caps_calloc(
        kIdHashSlots, sizeof(uint16_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
#else
    idSlots_ = static_cast<uint16_t *>(
        calloc(kIdHashSlots, sizeof(uint16_t)));
#endif
  }
  return ids_ != nullptr && idSlots_ != nullptr;
}

bool CapsuleOperationService::begin(fs::FS &fs, Print &log,
                                    StorageCoordinator &coordinator) {
  if (lifecycle_ != Lifecycle::uninitialized || !allocatePool() ||
      !journalStore_.begin(fs, kJournalDirectory) ||
      !transactionRunner_.begin(fs, log, coordinator)) return false;
  fs_ = &fs;
  log_ = &log;
  coordinator_ = &coordinator;
  ensureDirectoryIndex_ = 0;
  lifecycle_ = Lifecycle::ensureDirectories;
  diagnostic_ = "boot recovery pending";
  return true;
}

size_t CapsuleOperationService::fixedPoolBytes() const {
  return kPoolCapacity * kIdBytes + kIdHashSlots * sizeof(uint16_t);
}

size_t CapsuleOperationService::maximumPollBytes() const {
  size_t maximum = maximumPollBytes_;
  if (transactionRunner_.maximumPollBytes() > maximum) {
    maximum = transactionRunner_.maximumPollBytes();
  }
  return maximum;
}

bool CapsuleOperationService::readyForMutation() const {
  return lifecycle_ == Lifecycle::ready && !requestPending_ &&
      !outcomePending_ && catalog_ != nullptr;
}

bool CapsuleOperationService::mutationCapabilityBlocked() const {
  return lifecycle_ == Lifecycle::blocked;
}

bool CapsuleOperationService::busy() const {
  return requestPending_ || lifecycle_ == Lifecycle::running;
}

bool CapsuleOperationService::recoveryActive() const {
  return lifecycle_ == Lifecycle::ensureDirectories ||
      lifecycle_ == Lifecycle::transactionRecoveryStart ||
      lifecycle_ == Lifecycle::transactionRecovery ||
      lifecycle_ == Lifecycle::scanOpen ||
      lifecycle_ == Lifecycle::scanNext ||
      lifecycle_ == Lifecycle::scanClose ||
      lifecycle_ == Lifecycle::recoveryLoad ||
      lifecycle_ == Lifecycle::recoveryCheckpoint ||
      (lifecycle_ == Lifecycle::running && recoveryMode_);
}

const char *CapsuleOperationService::phaseName() const {
  switch (lifecycle_) {
    case Lifecycle::uninitialized: return "uninitialized";
    case Lifecycle::ensureDirectories: return "boot-directories";
    case Lifecycle::transactionRecoveryStart:
    case Lifecycle::transactionRecovery: return "metadata-recovery";
    case Lifecycle::scanOpen:
    case Lifecycle::scanNext:
    case Lifecycle::scanClose: return "boot-scan";
    case Lifecycle::recoveryLoad:
    case Lifecycle::recoveryCheckpoint: return "boot-recovery";
    case Lifecycle::running: return recoveryMode_ ? "recovering" : "mutation";
    case Lifecycle::ready: return "ready";
    case Lifecycle::blocked: return "blocked";
  }
  return "invalid";
}

void CapsuleOperationService::resetRequest() {
  itemCount_ = 0;
  changedAt_ = "";
  transactionId_ = "";
  requestPending_ = false;
  recoveryMode_ = false;
  pending_ = Pending::none;
  pendingStep_ = 0;
  cleanupStep_ = 0;
  cleanupIndex_ = 0;
  notificationIndex_ = 0;
  planReady_ = false;
  purgeEntryUnknown_ = false;
  observedSourceExists_ = false;
  observedTargetExists_ = false;
  metadataText_ = "";
  metadataValue_ = "";
  metadataPath_ = "";
  desiredFolder_ = "";
  failedId_ = "";
  work_ = {};
  plan_ = {};
  snapshot_ = {};
  if (ids_ != nullptr) memset(ids_, 0, kPoolCapacity * kIdBytes);
  if (idSlots_ != nullptr) memset(idSlots_, 0,
                                  kIdHashSlots * sizeof(uint16_t));
}

const char *CapsuleOperationService::idAt(size_t index) const {
  return ids_ != nullptr && index < itemCount_ ? ids_[index] : "";
}

bool CapsuleOperationService::copyRequestIds(
    const std::vector<String> &ids) {
  if (ids.empty() || ids.size() > kTransactionLimit ||
      ids.size() > kPoolCapacity || ids_ == nullptr || idSlots_ == nullptr) {
    return false;
  }
  memset(ids_, 0, kPoolCapacity * kIdBytes);
  memset(idSlots_, 0, kIdHashSlots * sizeof(uint16_t));
  for (size_t index = 0; index < ids.size(); ++index) {
    if (!isUuid(ids[index].c_str())) return false;
    uint32_t slot = idHash(ids[index].c_str()) & (kIdHashSlots - 1U);
    bool inserted = false;
    for (size_t probe = 0; probe < kIdHashSlots; ++probe) {
      const uint16_t stored = idSlots_[slot];
      if (stored == 0) {
        strlcpy(ids_[index], ids[index].c_str(), kIdBytes);
        idSlots_[slot] = static_cast<uint16_t>(index + 1U);
        inserted = true;
        break;
      }
      if (strcmp(ids_[stored - 1U], ids[index].c_str()) == 0) return false;
      slot = (slot + 1U) & (kIdHashSlots - 1U);
    }
    if (!inserted) return false;
  }
  itemCount_ = ids.size();
  return true;
}

bool CapsuleOperationService::submit(CapsuleOperationAction action,
                                     const std::vector<String> &ids,
                                     const String &changedAt) {
  if (!readyForMutation() || !copyRequestIds(ids) ||
      (action == CapsuleOperationAction::trash && changedAt.isEmpty())) {
    return false;
  }
  action_ = action;
  changedAt_ = changedAt;
  requestPending_ = true;
  diagnostic_ = "queued";
  return true;
}

bool CapsuleOperationService::takeOutcome(CapsuleOperationOutcome &outcome) {
  if (!outcomePending_) return false;
  outcome = outcome_;
  outcomePending_ = false;
  if (lifecycle_ == Lifecycle::ready) resetRequest();
  return true;
}

bool CapsuleOperationService::ensureOneDirectory() {
  static constexpr const char *directories[] = {
      kCapsuleRoot,
      kCapsuleSystem,
      kTransactionRoot,
      kJournalDirectory,
  };
  if (ensureDirectoryIndex_ >= sizeof(directories) / sizeof(directories[0])) {
    lifecycle_ = Lifecycle::transactionRecoveryStart;
    startupFailures_ = 0;
    return true;
  }
  StorageIoLease lease = coordinator_->acquireIo(
      StorageOwner::capsuleTransaction, StorageAccess::mutation, 0);
  if (!lease) return false;
  const char *path = directories[ensureDirectoryIndex_];
  if (!fs_->exists(path) && !fs_->mkdir(path)) {
    if (++startupFailures_ >= kMaximumPermanentFailures) {
      enterBlocked("local operation journal directory unavailable");
    }
    return false;
  }
  startupFailures_ = 0;
  ++ensureDirectoryIndex_;
  return true;
}

void CapsuleOperationService::pollStartup() {
  if (lifecycle_ == Lifecycle::ensureDirectories) {
    (void)ensureOneDirectory();
    return;
  }
  if (lifecycle_ == Lifecycle::transactionRecoveryStart) {
    if (!transactionRunner_.startRecovery(StorageOwner::recovery)) {
      if (++startupFailures_ >= kMaximumPermanentFailures) {
        enterBlocked("metadata transaction recovery could not start");
      }
      return;
    }
    startupFailures_ = 0;
    lifecycle_ = Lifecycle::transactionRecovery;
    return;
  }
  if (lifecycle_ == Lifecycle::transactionRecovery) {
    const CapsuleTransactionPollResult result = transactionRunner_.poll(0);
    if (transactionRunner_.lastPollBytes() > maximumPollBytes_) {
      maximumPollBytes_ = transactionRunner_.lastPollBytes();
    }
    if (result == CapsuleTransactionPollResult::progress ||
        result == CapsuleTransactionPollResult::wouldBlock) return;
    if (result != CapsuleTransactionPollResult::recovered) {
      enterBlocked("metadata transaction recovery retained authority");
      return;
    }
    lifecycle_ = Lifecycle::scanOpen;
    return;
  }
  if (lifecycle_ == Lifecycle::scanOpen) {
    StorageIoLease lease = coordinator_->acquireIo(
        StorageOwner::capsuleTransaction, StorageAccess::read, 0);
    if (!lease) return;
    startupDirectory_ = fs_->open(kJournalDirectory, FILE_READ);
    if (!startupDirectory_ || !startupDirectory_.isDirectory()) {
      if (startupDirectory_) startupDirectory_.close();
      if (++startupFailures_ >= kMaximumPermanentFailures) {
        enterBlocked("local operation recovery scan unavailable");
      }
      return;
    }
    startupFailures_ = 0;
    lifecycle_ = Lifecycle::scanNext;
    return;
  }
  if (lifecycle_ == Lifecycle::scanNext) {
    StorageIoLease lease = coordinator_->acquireIo(
        StorageOwner::capsuleTransaction, StorageAccess::read, 0);
    if (!lease) return;
    File entry = startupDirectory_.openNextFile();
    if (!entry) {
      lifecycle_ = Lifecycle::scanClose;
      return;
    }
    const String name = operationLeafName(entry.name());
    const bool isDirectory = entry.isDirectory();
    entry.close();
    if (!isDirectory && name.endsWith(".cbj") && name.length() == 40) {
      const String candidate = name.substring(0, 36);
      if (capsuleBatchUuid(candidate.c_str())) {
        startupCandidate_ = candidate;
        lifecycle_ = Lifecycle::scanClose;
      }
    }
    return;
  }
  if (lifecycle_ == Lifecycle::scanClose) {
    StorageIoLease lease = coordinator_->acquireIo(
        StorageOwner::capsuleTransaction, StorageAccess::read, 0);
    if (!lease) return;
    if (startupDirectory_) startupDirectory_.close();
    if (startupCandidate_.isEmpty()) {
      lifecycle_ = Lifecycle::ready;
      diagnostic_ = "ready";
    } else {
      lifecycle_ = Lifecycle::recoveryLoad;
    }
    return;
  }
  if (lifecycle_ == Lifecycle::recoveryLoad) {
    (void)startRecovery(startupCandidate_);
    return;
  }
  if (lifecycle_ == Lifecycle::recoveryCheckpoint) {
    if (!journalStore_.checkpoint(journalState_,
                                  StorageOwner::capsuleTransaction)) {
      if (++startupFailures_ >= kMaximumPermanentFailures) {
        reservation_.release();
        enterBlocked("local operation recovery checkpoint blocked");
      }
      return;
    }
    CapsuleBatchExecutor::Phase phase = CapsuleBatchExecutor::Phase::cleanup;
    if (journalState_.phase == CapsuleBatchPhase::rollback) {
      phase = CapsuleBatchExecutor::Phase::rollback;
    } else if (journalState_.phase == CapsuleBatchPhase::result) {
      phase = CapsuleBatchExecutor::Phase::result;
    }
    if (!executor_.resume(
            journalState_.total, phase, journalState_.cursor,
            journalState_.applied,
            (journalState_.flags & capsuleBatchSuccess) != 0,
            (journalState_.flags & capsuleBatchRollbackFailed) != 0)) {
      reservation_.release();
      enterBlocked("local operation recovery state rejected");
      return;
    }
    transactionId_ = journalState_.transactionId;
    recoveryMode_ = true;
    lifecycle_ = Lifecycle::running;
    diagnostic_ = "recovering interrupted local operation";
  }
}

bool CapsuleOperationService::startRecovery(const String &transactionId) {
  if (!reservation_) {
    reservation_ = coordinator_->reserve(StorageOwner::capsuleTransaction,
                                         StorageAccess::mutation, 0);
    if (!reservation_) return false;
  }
  StoredCapsuleBatchState loaded;
  const CapsuleBatchJournalStore::LoadResult result =
      journalStore_.loadStatus(transactionId,
                               StorageOwner::capsuleTransaction, loaded);
  if (result == CapsuleBatchJournalStore::LoadResult::wouldBlock) return false;
  if (result == CapsuleBatchJournalStore::LoadResult::invalid) {
    if (++startupFailures_ < kMaximumPermanentFailures) return false;
    char suffix[24];
    snprintf(suffix, sizeof(suffix), ".blocked-local");
    const bool quarantined = journalStore_.quarantine(
        transactionId, suffix, StorageOwner::capsuleTransaction);
    reservation_.release();
    if (!quarantined) {
      enterBlocked("invalid local operation authority could not be quarantined");
      return false;
    }
    startupFailures_ = 0;
    startupCandidate_ = "";
    lifecycle_ = Lifecycle::scanOpen;
    return true;
  }
  startupFailures_ = 0;
  prepareCapsuleBatchRecovery(loaded);
  loaded.flags &= ~capsuleBatchResponseAllowed;
  if (loaded.phase == CapsuleBatchPhase::preflight) {
    loaded.flags &= ~capsuleBatchSuccess;
    loaded.cursor = loaded.applied = 0;
    loaded.phase = CapsuleBatchPhase::result;
  } else if (loaded.phase == CapsuleBatchPhase::apply) {
    loaded.flags &= ~capsuleBatchSuccess;
    loaded.cursor = loaded.applied;
    loaded.phase = loaded.cursor == 0
        ? CapsuleBatchPhase::result : CapsuleBatchPhase::rollback;
  }
  sealCapsuleBatchState(loaded);
  journalState_ = loaded;
  lifecycle_ = Lifecycle::recoveryCheckpoint;
  return true;
}

void CapsuleOperationService::enterBlocked(const char *diagnostic) {
  if (metadataFile_) metadataFile_.close();
  if (purgeDirectory_) purgeDirectory_.close();
  if (startupDirectory_) startupDirectory_.close();
  reservation_.release();
  lifecycle_ = Lifecycle::blocked;
  diagnostic_ = diagnostic == nullptr ? "local operation blocked" : diagnostic;
  if (log_ != nullptr) {
    log_->printf("{\"event\":\"local_capsule_operation_blocked\","
                 "\"reason\":\"%s\"}\n", diagnostic_.c_str());
  }
}

void CapsuleOperationService::startSubmittedOperation() {
  if (!requestPending_ || lifecycle_ != Lifecycle::ready) return;
  if (!reservation_) {
    reservation_ = coordinator_->reserve(StorageOwner::capsuleTransaction,
                                         StorageAccess::mutation, 0);
    if (!reservation_) return;
  }
  transactionId_ = uuidForLocalOperation();
  if (!journalStore_.create(transactionId_, operationName(),
                            static_cast<uint16_t>(itemCount_),
                            StorageOwner::capsuleTransaction, journalState_) ||
      !executor_.begin(itemCount_)) {
    reservation_.release();
    startupCandidate_ = transactionId_;
    lifecycle_ = Lifecycle::recoveryLoad;
    requestPending_ = false;
    failedId_ = itemCount_ == 0 ? String() : String(idAt(0));
    diagnostic_ = "local operation journal creation failed";
    return;
  }
  requestPending_ = false;
  recoveryMode_ = false;
  lifecycle_ = Lifecycle::running;
  diagnostic_ = "running";
}

void CapsuleOperationService::poll(uint32_t nowMs) {
  if (lifecycle_ == Lifecycle::uninitialized ||
      lifecycle_ == Lifecycle::blocked) return;
  if (lifecycle_ != Lifecycle::ready && lifecycle_ != Lifecycle::running) {
    pollStartup();
    return;
  }
  if (lifecycle_ == Lifecycle::ready) {
    if (requestPending_) startSubmittedOperation();
    return;
  }
  advanceExecutor(nowMs);
}

void CapsuleOperationService::advanceExecutor(uint32_t nowMs) {
  if (pending_ != Pending::none) {
    advancePending(nowMs);
    return;
  }
  const CapsuleBatchExecutor::Work work = executor_.poll(true);
  if (work.action == CapsuleBatchExecutor::Action::none) return;
  beginWork(work);
}

void CapsuleOperationService::beginWork(
    const CapsuleBatchExecutor::Work &work) {
  work_ = work;
  pendingStep_ = 0;
  observedSourceExists_ = false;
  observedTargetExists_ = false;
  switch (work.action) {
    case CapsuleBatchExecutor::Action::checkpoint:
      finishWork(checkpoint(executor_.checkpointMarksItemInFlight()));
      return;
    case CapsuleBatchExecutor::Action::preflightItem:
      pending_ = Pending::preflight;
      return;
    case CapsuleBatchExecutor::Action::applyItem:
    case CapsuleBatchExecutor::Action::rollbackItem:
      if (!journalStore_.readPlan(
              journalState_, static_cast<uint16_t>(work.index), plan_,
              StorageOwner::capsuleTransaction)) {
        failedId_ = work.index < itemCount_ ? idAt(work.index) : "";
        finishWork(false);
        return;
      }
      if (recoveryMode_) {
        if (strcmp(journalState_.operation, "deleteCapsules") == 0) {
          action_ = CapsuleOperationAction::trash;
        } else if (strcmp(journalState_.operation, "restoreCapsules") == 0) {
          action_ = CapsuleOperationAction::restore;
        } else if (strcmp(journalState_.operation, "purgeCapsules") == 0) {
          action_ = CapsuleOperationAction::purge;
        } else if (strcmp(journalState_.operation, "moveCapsules") == 0) {
          const String archivePrefix = String(kCapsuleArchive) + "/";
          action_ = String(plan_.source).startsWith(archivePrefix.c_str())
              ? CapsuleOperationAction::unarchive
              : CapsuleOperationAction::archive;
        }
      }
      pending_ = work.action == CapsuleBatchExecutor::Action::applyItem
          ? Pending::applyPath : Pending::rollbackPath;
      return;
    case CapsuleBatchExecutor::Action::persistResult:
      // The preceding checkpoint is the durable result authority. Local UI
      // consumes an in-memory outcome only after cleanup reaches terminal.
      finishWork(true);
      return;
    case CapsuleBatchExecutor::Action::cleanup:
      pending_ = Pending::cleanup;
      cleanupIndex_ = 0;
      notificationIndex_ = 0;
      cleanupStep_ = 0;
      return;
    case CapsuleBatchExecutor::Action::finish:
      {
      const bool authorityPreserved = executor_.preserveJournal();
      publishOutcome();
      executor_.completeStep(true);
      reservation_.release();
      pending_ = Pending::none;
      if (authorityPreserved) {
        lifecycle_ = Lifecycle::blocked;
        diagnostic_ = "local operation authority preserved";
        if (log_ != nullptr) {
          log_->println(
              "{\"event\":\"local_capsule_operation_authority_preserved\"}");
        }
        return;
      }
      if (recoveryMode_) {
        recoveryMode_ = false;
        startupCandidate_ = "";
        transactionId_ = "";
        lifecycle_ = Lifecycle::scanOpen;
        diagnostic_ = "recovery complete";
      } else {
        lifecycle_ = Lifecycle::ready;
        diagnostic_ = "ready";
      }
      return;
      }
    default:
      finishWork(false);
      return;
  }
}

void CapsuleOperationService::advancePending(uint32_t nowMs) {
  switch (pending_) {
    case Pending::preflight:
      if (pendingStep_ == 0) {
        if (!buildPreflightPlan(work_.index)) finishWork(false);
        return;
      }
      if (pendingStep_ == 10) {
        if (!pollPurgeValidation()) return;
        if (purgeEntryUnknown_) {
          failedId_ = plan_.id;
          diagnostic_ = "purge contains unknown file or directory";
          finishWork(false);
          return;
        }
        pendingStep_ = 11;
        return;
      }
      if (pendingStep_ == 11 || pendingStep_ == 12) {
        StorageIoLease lease = coordinator_->acquireIo(
            StorageOwner::capsuleTransaction, StorageAccess::mutation, 0);
        if (!lease) return;
        const String path = pendingStep_ == 11
            ? String(kCapsuleStaging) : purgeTransactionDirectory();
        if (!fs_->exists(path) && !fs_->mkdir(path)) {
          failedId_ = plan_.id;
          diagnostic_ = "purge staging directory unavailable";
          finishWork(false);
          return;
        }
        pendingStep_ = pendingStep_ == 11 ? 12 : 20;
        return;
      }
      if (pendingStep_ == 20) {
        if (!pathExists(plan_.source, observedSourceExists_)) return;
        if (!observedSourceExists_) {
          failedId_ = plan_.id;
          finishWork(false);
          return;
        }
        pendingStep_ = plan_.target[0] == '\0' ? 23 : 21;
        return;
      }
      if (pendingStep_ == 21) {
        if (!pathExists(plan_.target, observedTargetExists_)) return;
        if (observedTargetExists_ &&
            (action_ == CapsuleOperationAction::unarchive ||
             action_ == CapsuleOperationAction::restore) &&
            !isInboxTarget(plan_.target, plan_.id)) {
          const String fallback = String(kCapsuleInbox) + "/" + plan_.id;
          strlcpy(plan_.target, fallback.c_str(), sizeof(plan_.target));
          observedTargetExists_ = false;
          pendingStep_ = 21;
          return;
        }
        if (observedTargetExists_) {
          failedId_ = plan_.id;
          diagnostic_ = "capsule destination already exists";
          finishWork(false);
          return;
        }
        pendingStep_ = 23;
        return;
      }
      if (pendingStep_ == 23) {
        finishWork(writeCurrentPlan());
        return;
      }
      finishWork(false);
      return;
    case Pending::metadataRead:
      if (continueMetadataRead() && !preparePlanAfterMetadata()) {
        finishWork(false);
      }
      return;
    case Pending::metadataCommit:
      if (pollMetadataCommit(nowMs)) pending_ = Pending::applyPath;
      return;
    case Pending::applyPath:
      if ((action_ == CapsuleOperationAction::archive ||
           action_ == CapsuleOperationAction::trash) && pendingStep_ == 0) {
        if (!startMetadataCommit(false)) {
          finishWork(false);
        }
        return;
      }
      if (advancePathMutation(false)) finishWork(true);
      return;
    case Pending::rollbackPath:
      if (advancePathMutation(true)) finishWork(true);
      return;
    case Pending::cleanup:
      if (advanceCleanup()) finishWork(true);
      return;
    case Pending::none:
      return;
  }
}

void CapsuleOperationService::finishWork(bool ok) {
  if (!ok && failedId_.isEmpty() && work_.index < itemCount_) {
    failedId_ = idAt(work_.index);
  }
  pending_ = Pending::none;
  pendingStep_ = 0;
  if (metadataFile_) metadataFile_.close();
  if (purgeDirectory_) purgeDirectory_.close();
  executor_.completeStep(ok);
}

bool CapsuleOperationService::buildPreflightPlan(size_t index) {
  if (catalog_ == nullptr || index >= itemCount_ ||
      !catalog_->operationSnapshot(idAt(index), snapshot_) ||
      strcmp(snapshot_.id, idAt(index)) != 0 || snapshot_.readOnly ||
      snapshot_.transcribing ||
      !capsuleBatchCapsulePath(snapshot_.directory, true)) {
    failedId_ = index < itemCount_ ? idAt(index) : "";
    diagnostic_ = "capsule is missing, busy, or read-only";
    return false;
  }
  plan_ = {};
  strlcpy(plan_.id, snapshot_.id, sizeof(plan_.id));
  strlcpy(plan_.source, snapshot_.directory, sizeof(plan_.source));
  plan_.expectedRevision = snapshot_.revision;

  if (action_ == CapsuleOperationAction::archive) {
    if (snapshot_.archived || snapshot_.trashed ||
        !safeArchiveOriginalFolder(snapshot_.folder)) return false;
    const String target = String(kCapsuleArchive) + "/" + snapshot_.id;
    strlcpy(plan_.target, target.c_str(), sizeof(plan_.target));
  } else if (action_ == CapsuleOperationAction::unarchive) {
    if (!snapshot_.archived || snapshot_.trashed) return false;
    metadataPath_ = String(snapshot_.directory) + "/" +
        kCapsuleArchiveMetadata;
    pending_ = Pending::metadataRead;
    pendingStep_ = 0;
    return true;
  } else if (action_ == CapsuleOperationAction::trash) {
    if (snapshot_.trashed || changedAt_.isEmpty()) return false;
    const String target = String(kCapsuleTrash) + "/" + snapshot_.id;
    strlcpy(plan_.target, target.c_str(), sizeof(plan_.target));
  } else if (action_ == CapsuleOperationAction::restore) {
    if (!snapshot_.trashed) return false;
    metadataPath_ = String(snapshot_.directory) + "/trash.json";
    pending_ = Pending::metadataRead;
    pendingStep_ = 0;
    return true;
  } else {
    if (!snapshot_.trashed ||
        String(snapshot_.directory) !=
            String(kCapsuleTrash) + "/" + snapshot_.id) return false;
    pendingStep_ = 10;
    return true;
  }
  sealCapsuleBatchPlan(plan_);
  if (!validCapsuleBatchPlan(plan_, operationName())) return false;
  pendingStep_ = 20;
  return true;
}

bool CapsuleOperationService::continueMetadataRead() {
  if (pendingStep_ == 0) {
    StorageIoLease lease = coordinator_->acquireIo(
        StorageOwner::capsuleTransaction, StorageAccess::read, 0);
    if (!lease) return false;
    metadataFile_ = fs_->open(metadataPath_, FILE_READ);
    if (!metadataFile_ || metadataFile_.isDirectory() ||
        metadataFile_.size() == 0 ||
        metadataFile_.size() > kMaximumOperationMetadataBytes) {
      if (metadataFile_) metadataFile_.close();
      failedId_ = plan_.id;
      diagnostic_ = "operation metadata is missing or invalid";
      return true;
    }
    metadataText_ = "";
    if (!metadataText_.reserve(metadataFile_.size() + 1)) return true;
    pendingStep_ = 1;
    return false;
  }
  if (pendingStep_ == 1) {
    StorageIoLease lease = coordinator_->acquireIo(
        StorageOwner::capsuleTransaction, StorageAccess::read, 0);
    if (!lease) return false;
    uint8_t buffer[4096];
    const size_t wanted = metadataFile_.available() <
            static_cast<int>(sizeof(buffer))
        ? static_cast<size_t>(metadataFile_.available()) : sizeof(buffer);
    if (wanted == 0) {
      pendingStep_ = 2;
      return false;
    }
    const int received = metadataFile_.read(buffer, wanted);
    if (received <= 0 || static_cast<size_t>(received) > wanted) return true;
    const size_t bytes = static_cast<size_t>(received);
    metadataText_.concat(reinterpret_cast<const char *>(buffer), bytes);
    if (bytes > maximumMetadataReadBytes_) maximumMetadataReadBytes_ = bytes;
    if (bytes > maximumPollBytes_) maximumPollBytes_ = bytes;
    if (metadataText_.length() > kMaximumOperationMetadataBytes) return true;
    if (!metadataFile_.available()) pendingStep_ = 2;
    return false;
  }
  StorageIoLease lease = coordinator_->acquireIo(
      StorageOwner::capsuleTransaction, StorageAccess::read, 0);
  if (!lease) return false;
  if (metadataFile_) metadataFile_.close();
  pendingStep_ = 3;
  return true;
}

bool CapsuleOperationService::preparePlanAfterMetadata() {
  String folder;
  if (pendingStep_ != 3 || !extractOriginalFolder(metadataText_, folder)) {
    failedId_ = plan_.id;
    diagnostic_ = "operation metadata originalFolder is invalid";
    return false;
  }
  String base = String(kCapsuleRoot) + "/" + folder;
  if (folder == "Inbox") base = kCapsuleInbox;
  const String target = base + "/" + plan_.id;
  strlcpy(plan_.target, target.c_str(), sizeof(plan_.target));
  sealCapsuleBatchPlan(plan_);
  if (!validCapsuleBatchPlan(plan_, operationName())) return false;
  pending_ = Pending::preflight;
  pendingStep_ = 20;
  metadataText_ = "";
  return true;
}

bool CapsuleOperationService::writeCurrentPlan() {
  sealCapsuleBatchPlan(plan_);
  return validCapsuleBatchPlan(plan_, operationName()) &&
      journalStore_.writePlan(journalState_,
                              static_cast<uint16_t>(work_.index), plan_,
                              StorageOwner::capsuleTransaction);
}

bool CapsuleOperationService::startMetadataCommit(bool rollback) {
  (void)rollback;
  const String folder = folderFromCapsulePath(plan_.source);
  if (!safeArchiveOriginalFolder(folder.c_str())) return false;
  if (action_ == CapsuleOperationAction::archive) {
    metadataValue_ = String("{\"schemaVersion\":1,\"capsuleId\":\"") +
        plan_.id + "\",\"originalFolder\":\"" + jsonEscaped(folder) +
        "\"}\n";
    metadataPath_ = String(plan_.source) + "/" + kCapsuleArchiveMetadata;
  } else {
    metadataValue_ = String("{\"schemaVersion\":1,\"capsuleId\":\"") +
        plan_.id + "\",\"trashedAt\":\"" + jsonEscaped(changedAt_) +
        "\",\"originalFolder\":\"" + jsonEscaped(folder) +
        "\",\"revision\":1}\n";
    metadataPath_ = String(plan_.source) + "/trash.json";
  }
  byteSource_.bind(metadataValue_);
  const CapsuleTransactionInput input{metadataPath_, &byteSource_, String()};
  const String key = transactionId_ + "-local-" + String(work_.index) +
      "-metadata";
  if (!transactionRunner_.startCommit(key.c_str(), &input, 1,
                                      StorageOwner::capsuleTransaction)) {
    return false;
  }
  pending_ = Pending::metadataCommit;
  pendingStep_ = 1;
  return true;
}

bool CapsuleOperationService::pollMetadataCommit(uint32_t nowMs) {
  const CapsuleTransactionPollResult result = transactionRunner_.poll(nowMs);
  if (transactionRunner_.lastPollBytes() > maximumPollBytes_) {
    maximumPollBytes_ = transactionRunner_.lastPollBytes();
  }
  if (result == CapsuleTransactionPollResult::progress ||
      result == CapsuleTransactionPollResult::wouldBlock) return false;
  if (result != CapsuleTransactionPollResult::committed) {
    finishWork(false);
    return false;
  }
  pendingStep_ = 2;
  return true;
}

bool CapsuleOperationService::advancePathMutation(bool rollback) {
  String source = plan_.source;
  String target = plan_.target;
  if (action_ == CapsuleOperationAction::purge) {
    target = purgeItemPath(plan_);
  }
  if (!rollback) {
    if (pendingStep_ < 2 &&
        (action_ == CapsuleOperationAction::archive ||
         action_ == CapsuleOperationAction::trash)) return false;
    if (!renamePath(source, target)) {
      diagnostic_ = "capsule path mutation failed";
      finishWork(false);
      return false;
    }
    return true;
  }

  if (pendingStep_ == 0) {
    if (!pathExists(source, observedSourceExists_)) return false;
    pendingStep_ = 1;
    return false;
  }
  if (pendingStep_ == 1) {
    if (!pathExists(target, observedTargetExists_)) return false;
    pendingStep_ = 2;
    return false;
  }
  if (pendingStep_ == 2) {
    if (!observedSourceExists_ && observedTargetExists_) {
      if (!renamePath(target, source)) {
        finishWork(false);
        return false;
      }
    } else if (!observedSourceExists_ || observedTargetExists_) {
      finishWork(false);
      return false;
    }
    pendingStep_ = 3;
    return false;
  }
  if (pendingStep_ == 3 &&
      (action_ == CapsuleOperationAction::archive ||
       action_ == CapsuleOperationAction::trash)) {
    const String metadata = source +
        (action_ == CapsuleOperationAction::archive
             ? String("/") + kCapsuleArchiveMetadata : "/trash.json");
    if (!removePathIfPresent(metadata)) {
      finishWork(false);
      return false;
    }
  }
  return true;
}

bool CapsuleOperationService::advanceCleanup() {
  if (executor_.preserveJournal()) return true;
  if (executor_.success() && catalog_ != nullptr &&
      notificationIndex_ < executor_.total()) {
    StoredCapsuleBatchPlan committedPlan;
    if (!journalStore_.readPlan(
            journalState_, static_cast<uint16_t>(notificationIndex_),
            committedPlan, StorageOwner::capsuleTransaction)) return false;
    const bool removed =
        strcmp(journalState_.operation, "purgeCapsules") == 0;
    if (!catalog_->operationCommitted(
            committedPlan.id, committedPlan.target, removed)) {
      diagnostic_ = "durable operation committed; index refresh queued";
    }
    ++notificationIndex_;
    return false;
  }
  if (executor_.success() &&
      (strcmp(journalState_.operation, "moveCapsules") == 0 ||
       strcmp(journalState_.operation, "restoreCapsules") == 0)) {
    while (cleanupIndex_ < executor_.total()) {
      if (cleanupStep_ == 0) {
        if (!journalStore_.readPlan(
                journalState_, static_cast<uint16_t>(cleanupIndex_), plan_,
                StorageOwner::capsuleTransaction)) return false;
        cleanupStep_ = 1;
        return false;
      }
      const bool unarchive = String(plan_.source).startsWith(
          (String(kCapsuleArchive) + "/").c_str());
      const bool restore = String(plan_.source).startsWith(
          (String(kCapsuleTrash) + "/").c_str());
      if (unarchive || restore) {
        const String metadata = String(plan_.target) +
            (restore ? "/trash.json" : String("/") + kCapsuleArchiveMetadata);
        if (!removePathIfPresent(metadata)) return false;
      }
      ++cleanupIndex_;
      cleanupStep_ = 0;
      return false;
    }
  }
  if (executor_.success() &&
      strcmp(journalState_.operation, "purgeCapsules") == 0) {
    if (cleanupIndex_ < executor_.total()) {
      if (!treeStepper_.active() && cleanupStep_ == 0) {
        if (!journalStore_.readPlan(
                journalState_, static_cast<uint16_t>(cleanupIndex_), plan_,
                StorageOwner::capsuleTransaction) ||
            !treeStepper_.beginRemove(*fs_, purgeItemPath(plan_),
                                      StorageOwner::capsuleTransaction)) {
          return false;
        }
        cleanupStep_ = 1;
        return false;
      }
      const LinkTreeStepper::Result result = treeStepper_.poll();
      if (result == LinkTreeStepper::Result::progress ||
          result == LinkTreeStepper::Result::wouldBlock) return false;
      if (result != LinkTreeStepper::Result::complete) return false;
      ++cleanupIndex_;
      cleanupStep_ = 0;
      return false;
    }
    if (cleanupStep_ < 3) {
      StorageIoLease lease = coordinator_->acquireIo(
          StorageOwner::capsuleTransaction, StorageAccess::mutation, 0);
      if (!lease) return false;
      const String staging = purgeTransactionDirectory();
      if (fs_->exists(staging) && !fs_->rmdir(staging)) return false;
      cleanupStep_ = 3;
      return false;
    }
  }
  const uint8_t journalPart = static_cast<uint8_t>(cleanupStep_ >= 10
      ? cleanupStep_ - 10 : 0);
  if (cleanupStep_ < 10) {
    cleanupStep_ = 10;
    return false;
  }
  if (journalPart < 3) {
    if (!journalStore_.erasePart(transactionId_, journalPart,
                                 StorageOwner::capsuleTransaction)) {
      return false;
    }
    ++cleanupStep_;
    return false;
  }
  return true;
}

bool CapsuleOperationService::pollPurgeValidation() {
  if (pendingStep_ != 10) return true;
  if (!purgeDirectory_) {
    StorageIoLease lease = coordinator_->acquireIo(
        StorageOwner::capsuleTransaction, StorageAccess::read, 0);
    if (!lease) return false;
    purgeDirectory_ = fs_->open(plan_.source, FILE_READ);
    if (!purgeDirectory_ || !purgeDirectory_.isDirectory()) {
      purgeEntryUnknown_ = true;
      if (purgeDirectory_) purgeDirectory_.close();
      return true;
    }
    return false;
  }
  StorageIoLease lease = coordinator_->acquireIo(
      StorageOwner::capsuleTransaction, StorageAccess::read, 0);
  if (!lease) return false;
  File entry = purgeDirectory_.openNextFile();
  if (!entry) {
    purgeDirectory_.close();
    return true;
  }
  const String leaf = operationLeafName(entry.name());
  const bool allowed = !entry.isDirectory() &&
      permittedPurgeLeaf(leaf.c_str());
  entry.close();
  if (!allowed) {
    purgeEntryUnknown_ = true;
    purgeDirectory_.close();
    return true;
  }
  return false;
}

String CapsuleOperationService::purgeTransactionDirectory() const {
  return String(kCapsuleStaging) + "/purge-local-" + transactionId_;
}

String CapsuleOperationService::purgeItemPath(
    const StoredCapsuleBatchPlan &plan) const {
  return purgeTransactionDirectory() + "/" + plan.id;
}

const char *CapsuleOperationService::operationName() const {
  switch (action_) {
    case CapsuleOperationAction::archive:
    case CapsuleOperationAction::unarchive: return "moveCapsules";
    case CapsuleOperationAction::trash: return "deleteCapsules";
    case CapsuleOperationAction::restore: return "restoreCapsules";
    case CapsuleOperationAction::purge: return "purgeCapsules";
  }
  return "moveCapsules";
}

void CapsuleOperationService::syncJournalState(bool itemInFlight) {
  switch (executor_.phase()) {
    case CapsuleBatchExecutor::Phase::preflight:
      journalState_.phase = CapsuleBatchPhase::preflight;
      break;
    case CapsuleBatchExecutor::Phase::apply:
    case CapsuleBatchExecutor::Phase::finalize:
      journalState_.phase = CapsuleBatchPhase::apply;
      break;
    case CapsuleBatchExecutor::Phase::rollback:
      journalState_.phase = CapsuleBatchPhase::rollback;
      break;
    case CapsuleBatchExecutor::Phase::result:
      journalState_.phase = CapsuleBatchPhase::result;
      break;
    default:
      journalState_.phase = CapsuleBatchPhase::cleanup;
      break;
  }
  journalState_.cursor = static_cast<uint16_t>(executor_.cursor());
  journalState_.applied = static_cast<uint16_t>(executor_.applied());
  journalState_.flags = 0;
  if (executor_.success()) journalState_.flags |= capsuleBatchSuccess;
  if (executor_.responseAllowed()) {
    journalState_.flags |= capsuleBatchResponseAllowed;
  }
  if (executor_.rollbackFailed()) {
    journalState_.flags |= capsuleBatchRollbackFailed;
  }
  if (itemInFlight) journalState_.flags |= capsuleBatchItemInFlight;
  sealCapsuleBatchState(journalState_);
}

bool CapsuleOperationService::checkpoint(bool itemInFlight) {
  syncJournalState(itemInFlight);
  return journalStore_.checkpoint(journalState_,
                                  StorageOwner::capsuleTransaction);
}

void CapsuleOperationService::publishOutcome() {
  if (recoveryMode_) return;
  outcome_ = {};
  outcome_.action = action_;
  outcome_.requested = itemCount_;
  outcome_.committed = executor_.success();
  outcome_.changed = executor_.success() ? itemCount_ : executor_.applied();
  outcome_.rollbackFailed = executor_.rollbackFailed();
  outcome_.authorityPreserved = executor_.preserveJournal();
  outcome_.failedId = failedId_;
  outcome_.diagnostic = diagnostic_;
  outcomePending_ = true;
  if (catalog_ != nullptr) {
    catalog_->operationFinished(
        reinterpret_cast<const char *>(ids_), kIdBytes, itemCount_,
        outcome_.committed, action_ == CapsuleOperationAction::purge);
  }
}

bool CapsuleOperationService::pathExists(const String &path, bool &exists,
                                         StorageAccess access) {
  StorageIoLease lease = coordinator_->acquireIo(
      StorageOwner::capsuleTransaction, access, 0);
  if (!lease) return false;
  exists = fs_->exists(path);
  return true;
}

bool CapsuleOperationService::renamePath(const String &source,
                                         const String &target) {
  if (source.isEmpty() || target.isEmpty()) return false;
  StorageIoLease lease = coordinator_->acquireIo(
      StorageOwner::capsuleTransaction, StorageAccess::mutation, 0);
  return lease && fs_->rename(source, target);
}

bool CapsuleOperationService::removePathIfPresent(const String &path) {
  StorageIoLease lease = coordinator_->acquireIo(
      StorageOwner::capsuleTransaction, StorageAccess::mutation, 0);
  if (!lease) return false;
  return !fs_->exists(path) || fs_->remove(path);
}

bool CapsuleOperationService::permittedPurgeLeaf(const char *leaf) {
  if (leaf == nullptr) return false;
  static constexpr const char *allowed[] = {
      "capsule.json", "processing.json", "audio.wav", "audio.m4a",
      "raw.txt", "final.md", "polished.md", "trash.json",
      "archive.json",
  };
  for (const char *candidate : allowed) {
    if (strcmp(leaf, candidate) == 0) return true;
  }
  return false;
}

bool CapsuleOperationService::extractOriginalFolder(const String &json,
                                                    String &folder) {
  const char *text = json.c_str();
  const char *key = strstr(text, "\"originalFolder\"");
  if (key == nullptr) return false;
  const char *cursor = strchr(key + strlen("\"originalFolder\""), ':');
  if (cursor == nullptr) return false;
  ++cursor;
  while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' ||
         *cursor == '\n') ++cursor;
  if (*cursor++ != '"') return false;
  String decoded;
  while (*cursor != '\0' && *cursor != '"') {
    char value = *cursor++;
    if (value == '\\') {
      const char escaped = *cursor++;
      if (escaped == '"' || escaped == '\\' || escaped == '/') {
        value = escaped;
      } else if (escaped == 'n') {
        value = '\n';
      } else if (escaped == 'r') {
        value = '\r';
      } else if (escaped == 't') {
        value = '\t';
      } else {
        return false;
      }
    }
    decoded += value;
    if (decoded.length() >= kCapsuleBatchFolderBytes) return false;
  }
  if (*cursor != '"' || !safeArchiveOriginalFolder(decoded.c_str())) {
    return false;
  }
  folder = decoded;
  return true;
}

String CapsuleOperationService::jsonEscaped(const String &value) {
  String result;
  result.reserve(value.length() + 8);
  for (const char *cursor = value.c_str(); *cursor != '\0'; ++cursor) {
    switch (*cursor) {
      case '"': result += "\\\""; break;
      case '\\': result += "\\\\"; break;
      case '\n': result += "\\n"; break;
      case '\r': result += "\\r"; break;
      case '\t': result += "\\t"; break;
      default:
        if (static_cast<unsigned char>(*cursor) < 0x20) return String();
        result += *cursor;
        break;
    }
  }
  return result;
}

}  // namespace pokepod
