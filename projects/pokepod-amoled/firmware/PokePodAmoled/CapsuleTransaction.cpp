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

enum class CapsuleTransactionRunner::Phase : uint8_t {
  idle = 0,
  reserve,
  rootExists,
  rootMkdir,
  systemExists,
  systemMkdir,
  transactionsExists,
  transactionsMkdir,
  commitCheckJournal,
  staleStagedExists,
  staleStagedRemove,
  staleBackupExists,
  staleBackupRemove,
  staleNext,
  staleJournalTemporaryExists,
  staleJournalTemporaryRemove,
  prepareTarget,
  openStage,
  writeStage,
  flushStage,
  closeStage,
  openPreparedFact,
  readFact,
  closeFact,
  targetExistsForJournal,
  nextTarget,
  openJournalTemporary,
  writeJournalTemporary,
  flushJournalTemporary,
  closeJournalTemporary,
  oldJournalExists,
  oldJournalRemove,
  publishJournal,
  publishPrepared,
  recoveryScanOpen,
  recoveryScanNext,
  recoveryScanCloseEntry,
  recoveryScanCloseDirectory,
  recoveryJournalOpen,
  recoveryJournalRead,
  recoveryJournalClose,
  recoveryTargetFactOpen,
  recoveryStagedFactOpen,
  recoveryBackupExists,
  recoveryFactsNext,
  recoveryDecide,
  commitTarget,
  commitBackupRemove,
  commitTargetRename,
  commitStagedRename,
  commitNext,
  rollbackTarget,
  rollbackTargetRemove,
  rollbackBackupRename,
  rollbackNewRemove,
  rollbackNext,
  cleanupCloseFile,
  cleanupCloseEntry,
  cleanupCloseDirectory,
  cleanupStagedExists,
  cleanupStagedRemove,
  cleanupBackupExists,
  cleanupBackupRemove,
  cleanupNext,
  cleanupJournalTemporaryExists,
  cleanupJournalTemporaryRemove,
  cleanupJournalExists,
  cleanupJournalRemove,
  cleanupDone,
};

namespace {

constexpr uint8_t kCapsuleTransactionPermanentFailurePolls = 3;

}  // namespace

bool CapsuleTransactionRunner::begin(fs::FS &fs, Print &log,
                                     StorageCoordinator &coordinator) {
  if (active() || reservation_) return false;
  fs_ = &fs;
  log_ = &log;
  coordinator_ = &coordinator;
  return true;
}

bool CapsuleTransactionRunner::validInput(
    const CapsuleTransactionInput &input) {
  const bool memorySource = input.source != nullptr;
  const bool preparedSource = !input.preparedPath.isEmpty();
  return capsuleTransactionPathValid(input.targetPath.c_str()) &&
      memorySource != preparedSource &&
      (!preparedSource ||
       capsuleTransactionPathValid(input.preparedPath.c_str()));
}

void CapsuleTransactionRunner::resetOperationFields() {
  file_ = File();
  directory_ = File();
  entry_ = File();
  mode_ = Mode::none;
  phase_ = Phase::idle;
  cleanupTerminal_ = CapsuleTransactionRunState::failed;
  owner_ = StorageOwner::none;
  for (uint8_t index = 0; index < kCapsuleTransactionMaximumTargets; ++index) {
    targets_[index] = {};
  }
  targetCount_ = 0;
  targetIndex_ = 0;
  cleanupIndex_ = 0;
  primitiveFailures_ = 0;
  factKind_ = FactKind::none;
  fileAccess_ = StorageAccess::read;
  journal_ = {};
  recoveryDecision_ = CapsuleTransactionRecovery::ambiguous;
  key_ = "";
  base_ = "";
  factPath_ = "";
  factLength_ = 0;
  factOffset_ = 0;
  factCrcState_ = 0xffffffffU;
  factExists_ = false;
  journalDurable_ = false;
  cancellationRequested_ = false;
  preserveJournal_ = false;
  cleanupPathExists_ = false;
  lastPollBytes_ = 0;
  maximumPollBytes_ = 0;
  maximumIoBytes_ = 0;
  polls_ = 0;
}

bool CapsuleTransactionRunner::startCommon(StorageOwner owner, Mode mode) {
  if (fs_ == nullptr || coordinator_ == nullptr || active() || reservation_ ||
      owner == StorageOwner::none) {
    return false;
  }
  resetOperationFields();
  mode_ = mode;
  owner_ = owner;
  phase_ = Phase::reserve;
  state_ = CapsuleTransactionRunState::running;
  return true;
}

bool CapsuleTransactionRunner::startCommit(
    const char *key, const CapsuleTransactionInput *targets, uint8_t count,
    StorageOwner owner) {
  if (targets == nullptr || count == 0 ||
      count > kCapsuleTransactionMaximumTargets) {
    return false;
  }
  for (uint8_t index = 0; index < count; ++index) {
    if (!validInput(targets[index])) return false;
  }
  if (!startCommon(owner, Mode::commit)) return false;
  key_ = key == nullptr ? "" : key;
  targetCount_ = count;
  for (uint8_t index = 0; index < count; ++index) {
    targets_[index].targetPath = targets[index].targetPath;
    targets_[index].source = targets[index].source;
    targets_[index].preparedPath = targets[index].preparedPath;
  }
  base_ = transactionBase(key_.c_str());
  return true;
}

bool CapsuleTransactionRunner::startPreparedFile(
    const char *key, const String &preparedPath, const String &targetPath,
    StorageOwner owner) {
  const CapsuleTransactionInput input{targetPath, nullptr, preparedPath};
  return startCommit(key, &input, 1, owner);
}

bool CapsuleTransactionRunner::startRecovery(StorageOwner owner) {
  return startCommon(owner, Mode::recoverAll);
}

String CapsuleTransactionRunner::transactionBase(const char *key) const {
  uint32_t hash = hashKey(
      key, targetCount_ == 0 ? nullptr : targets_[0].targetPath.c_str());
  if (targetCount_ > 1) {
    hash ^= hashKey(nullptr, targets_[1].targetPath.c_str());
  }
  char name[24];
  snprintf(name, sizeof(name), "/tx-%08lx",
           static_cast<unsigned long>(hash));
  return String(kTransactionDirectory) + name;
}

String CapsuleTransactionRunner::sidePath(uint8_t index,
                                          const char *suffix) const {
  return base_ + "." + String(index) + suffix;
}

const char *CapsuleTransactionRunner::phaseName() const {
  switch (phase_) {
    case Phase::idle: return "idle";
    case Phase::reserve: return "reserve";
    case Phase::writeStage: return "write-stage";
    case Phase::readFact: return "crc";
    case Phase::publishJournal: return "publish-journal";
    case Phase::publishPrepared: return "publish-prepared";
    case Phase::recoveryScanOpen:
    case Phase::recoveryScanNext:
    case Phase::recoveryScanCloseEntry:
    case Phase::recoveryScanCloseDirectory: return "scan";
    case Phase::recoveryJournalOpen:
    case Phase::recoveryJournalRead:
    case Phase::recoveryJournalClose: return "journal";
    case Phase::commitTarget:
    case Phase::commitBackupRemove:
    case Phase::commitTargetRename:
    case Phase::commitStagedRename:
    case Phase::commitNext: return "commit";
    case Phase::rollbackTarget:
    case Phase::rollbackTargetRemove:
    case Phase::rollbackBackupRename:
    case Phase::rollbackNewRemove:
    case Phase::rollbackNext: return "rollback";
    case Phase::cleanupCloseFile:
    case Phase::cleanupCloseEntry:
    case Phase::cleanupCloseDirectory:
    case Phase::cleanupStagedExists:
    case Phase::cleanupStagedRemove:
    case Phase::cleanupBackupExists:
    case Phase::cleanupBackupRemove:
    case Phase::cleanupNext:
    case Phase::cleanupJournalTemporaryExists:
    case Phase::cleanupJournalTemporaryRemove:
    case Phase::cleanupJournalExists:
    case Phase::cleanupJournalRemove:
    case Phase::cleanupDone: return "cleanup";
    default: return "prepare";
  }
}

CapsuleTransactionPollResult CapsuleTransactionRunner::resultForState() const {
  switch (state_) {
    case CapsuleTransactionRunState::idle:
      return CapsuleTransactionPollResult::idle;
    case CapsuleTransactionRunState::running:
      return CapsuleTransactionPollResult::progress;
    case CapsuleTransactionRunState::committed:
      return CapsuleTransactionPollResult::committed;
    case CapsuleTransactionRunState::recovered:
      return CapsuleTransactionPollResult::recovered;
    case CapsuleTransactionRunState::cancelled:
      return CapsuleTransactionPollResult::cancelled;
    case CapsuleTransactionRunState::failed:
      return CapsuleTransactionPollResult::failed;
    case CapsuleTransactionRunState::cleanupBlocked:
      return CapsuleTransactionPollResult::cleanupBlocked;
    case CapsuleTransactionRunState::recoveryBlocked:
      return CapsuleTransactionPollResult::recoveryBlocked;
  }
  return CapsuleTransactionPollResult::failed;
}

CapsuleTransactionPollResult CapsuleTransactionRunner::finishTerminal(
    CapsuleTransactionRunState state) {
  state_ = state;
  phase_ = Phase::idle;
  mode_ = Mode::none;
  owner_ = StorageOwner::none;
  reservation_.release();
  return resultForState();
}

CapsuleTransactionPollResult CapsuleTransactionRunner::primitiveFailed(
    bool cleanup) {
  ++primitiveFailures_;
  if (primitiveFailures_ < kCapsuleTransactionPermanentFailurePolls) {
    return CapsuleTransactionPollResult::wouldBlock;
  }
  primitiveFailures_ = 0;
  if (cleanup) {
    // Cleanup remove/rename phases have no open File handles. Retain the
    // journal/side artifacts as restart truth, but do not wedge every other
    // storage owner behind an inactive runner's reservation.
    return finishTerminal(CapsuleTransactionRunState::cleanupBlocked);
  }
  beginCleanup(journalDurable_ ?
                   CapsuleTransactionRunState::recoveryBlocked :
                   CapsuleTransactionRunState::failed,
               journalDurable_);
  return CapsuleTransactionPollResult::progress;
}

void CapsuleTransactionRunner::beginCleanup(
    CapsuleTransactionRunState terminalState, bool preserveJournal) {
  cleanupTerminal_ = terminalState;
  preserveJournal_ = preserveJournal;
  cleanupIndex_ = 0;
  primitiveFailures_ = 0;
  if (file_) {
    phase_ = Phase::cleanupCloseFile;
  } else if (entry_) {
    phase_ = Phase::cleanupCloseEntry;
  } else if (directory_) {
    phase_ = Phase::cleanupCloseDirectory;
  } else if (preserveJournal_) {
    phase_ = Phase::cleanupDone;
  } else {
    phase_ = Phase::cleanupStagedExists;
  }
}

CapsuleTransactionPollResult CapsuleTransactionRunner::closeHandle(
    Phase next, bool writing) {
  StorageIoLease lease = coordinator_->acquireIo(
      owner_, writing ? StorageAccess::mutation : StorageAccess::read, 0);
  if (!lease) return CapsuleTransactionPollResult::wouldBlock;
  const int errorBefore = writing && file_ ? file_.getWriteError() : 0;
  file_.close();
  const int errorAfter = writing ? file_.getWriteError() : 0;
  primitiveFailures_ = 0;
  if (errorBefore != 0 || errorAfter != 0) {
    beginCleanup(journalDurable_ ?
                     CapsuleTransactionRunState::recoveryBlocked :
                     CapsuleTransactionRunState::failed,
                 journalDurable_);
    return CapsuleTransactionPollResult::progress;
  }
  fileAccess_ = StorageAccess::read;
  phase_ = next;
  return CapsuleTransactionPollResult::progress;
}

CapsuleTransactionPollResult CapsuleTransactionRunner::openRead(
    const String &path, FactKind kind, Phase missing, Phase opened) {
  StorageIoLease lease = coordinator_->acquireIo(owner_, StorageAccess::read, 0);
  if (!lease) return CapsuleTransactionPollResult::wouldBlock;
  file_ = fs_->open(path, FILE_READ);
  primitiveFailures_ = 0;
  if (!file_) {
    factExists_ = false;
    factKind_ = kind;
    if (kind == FactKind::source) {
      beginCleanup(CapsuleTransactionRunState::failed, journalDurable_);
    } else {
      TargetRuntime &target = targets_[targetIndex_];
      if (kind == FactKind::target) {
        target.targetExists = false;
        target.targetMatches = false;
      } else if (kind == FactKind::staged) {
        target.stagedExists = false;
        target.stagedMatches = false;
      }
      phase_ = missing;
    }
    return CapsuleTransactionPollResult::progress;
  }
  fileAccess_ = StorageAccess::read;
  if (file_.isDirectory() || file_.size() > UINT32_MAX) {
    beginCleanup(journalDurable_ ?
                     CapsuleTransactionRunState::recoveryBlocked :
                     CapsuleTransactionRunState::failed,
                 journalDurable_);
    return CapsuleTransactionPollResult::progress;
  }
  factPath_ = path;
  factKind_ = kind;
  factExists_ = true;
  factLength_ = static_cast<uint32_t>(file_.size());
  factOffset_ = 0;
  factCrcState_ = 0xffffffffU;
  phase_ = opened;
  return CapsuleTransactionPollResult::progress;
}

CapsuleTransactionPollResult CapsuleTransactionRunner::readFactChunk() {
  const uint32_t remaining = factLength_ - factOffset_;
  if (remaining == 0) {
    phase_ = Phase::closeFact;
    return CapsuleTransactionPollResult::progress;
  }
  const size_t wanted = remaining < kCapsuleTransactionIoBytes
      ? static_cast<size_t>(remaining) : kCapsuleTransactionIoBytes;
  StorageIoLease lease = coordinator_->acquireIo(owner_, StorageAccess::read, 0);
  if (!lease) return CapsuleTransactionPollResult::wouldBlock;
  const int received = file_.read(ioBuffer_, wanted);
  if (received <= 0 || static_cast<size_t>(received) > wanted) {
    beginCleanup(journalDurable_ ?
                     CapsuleTransactionRunState::recoveryBlocked :
                     CapsuleTransactionRunState::failed,
                 journalDurable_);
    return CapsuleTransactionPollResult::progress;
  }
  const size_t bytes = static_cast<size_t>(received);
  lastPollBytes_ = bytes;
  if (bytes > maximumIoBytes_) maximumIoBytes_ = bytes;
  factOffset_ += static_cast<uint32_t>(bytes);
  factCrcState_ = capsuleTransactionCrc32Update(
      factCrcState_, ioBuffer_, bytes);
  primitiveFailures_ = 0;
  if (bytes != wanted && factOffset_ != factLength_) {
    beginCleanup(journalDurable_ ?
                     CapsuleTransactionRunState::recoveryBlocked :
                     CapsuleTransactionRunState::failed,
                 journalDurable_);
  } else if (factOffset_ == factLength_) {
    phase_ = Phase::closeFact;
  }
  return CapsuleTransactionPollResult::progress;
}

CapsuleTransactionPollResult CapsuleTransactionRunner::removeIfPresent(
    const String &path, Phase next) {
  StorageIoLease lease = coordinator_->acquireIo(
      owner_, StorageAccess::mutation, 0);
  if (!lease) return CapsuleTransactionPollResult::wouldBlock;
  if (!fs_->remove(path)) return primitiveFailed(true);
  primitiveFailures_ = 0;
  phase_ = next;
  return CapsuleTransactionPollResult::progress;
}

CapsuleTransactionPollResult CapsuleTransactionRunner::renamePath(
    const String &from, const String &to, Phase next) {
  StorageIoLease lease = coordinator_->acquireIo(
      owner_, StorageAccess::mutation, 0);
  if (!lease) return CapsuleTransactionPollResult::wouldBlock;
  if (!fs_->rename(from, to)) return primitiveFailed(journalDurable_);
  primitiveFailures_ = 0;
  phase_ = next;
  return CapsuleTransactionPollResult::progress;
}

CapsuleTransactionPollResult CapsuleTransactionRunner::poll(
    uint32_t nowMs, CapsuleTransactionGate *gate) {
  if (!active()) return resultForState();
  ++polls_;
  lastPollBytes_ = 0;
  if (!cancellationRequested_ &&
      !capsuleTransactionPermitted(gate, nowMs)) {
    cancellationRequested_ = true;
    if (!reservation_) {
      return finishTerminal(CapsuleTransactionRunState::cancelled);
    } else {
      beginCleanup(CapsuleTransactionRunState::cancelled,
                   journalDurable_ || mode_ == Mode::recoverAll);
    }
  }
  const CapsuleTransactionPollResult result = step();
  if (lastPollBytes_ > maximumPollBytes_) maximumPollBytes_ = lastPollBytes_;
  return result;
}

CapsuleTransactionPollResult CapsuleTransactionRunner::step() {
  switch (phase_) {
    case Phase::idle:
      return resultForState();
    case Phase::reserve: {
      StorageReservation reservation = coordinator_->reserve(
          owner_, StorageAccess::mutation, 0);
      if (!reservation) return CapsuleTransactionPollResult::wouldBlock;
      reservation_ = std::move(reservation);
      primitiveFailures_ = 0;
      phase_ = Phase::rootExists;
      return CapsuleTransactionPollResult::progress;
    }
    case Phase::rootExists:
    case Phase::systemExists:
    case Phase::transactionsExists: {
      const char *path = phase_ == Phase::rootExists ? kCapsuleRoot :
          (phase_ == Phase::systemExists ? kCapsuleSystem :
                                           kTransactionDirectory);
      StorageIoLease lease = coordinator_->acquireIo(
          owner_, StorageAccess::read, 0);
      if (!lease) return CapsuleTransactionPollResult::wouldBlock;
      const bool exists = fs_->exists(path);
      primitiveFailures_ = 0;
      if (phase_ == Phase::rootExists) {
        phase_ = exists ? Phase::systemExists : Phase::rootMkdir;
      } else if (phase_ == Phase::systemExists) {
        phase_ = exists ? Phase::transactionsExists : Phase::systemMkdir;
      } else {
        phase_ = exists ?
            (mode_ == Mode::commit ? Phase::commitCheckJournal :
                                     Phase::recoveryScanOpen) :
            Phase::transactionsMkdir;
      }
      return CapsuleTransactionPollResult::progress;
    }
    case Phase::rootMkdir:
    case Phase::systemMkdir:
    case Phase::transactionsMkdir: {
      const char *path = phase_ == Phase::rootMkdir ? kCapsuleRoot :
          (phase_ == Phase::systemMkdir ? kCapsuleSystem :
                                         kTransactionDirectory);
      StorageIoLease lease = coordinator_->acquireIo(
          owner_, StorageAccess::mutation, 0);
      if (!lease) return CapsuleTransactionPollResult::wouldBlock;
      if (!fs_->mkdir(path)) return primitiveFailed(false);
      primitiveFailures_ = 0;
      if (phase_ == Phase::rootMkdir) phase_ = Phase::systemExists;
      else if (phase_ == Phase::systemMkdir) {
        phase_ = Phase::transactionsExists;
      } else {
        phase_ = mode_ == Mode::commit ? Phase::commitCheckJournal :
                                        Phase::recoveryScanOpen;
      }
      return CapsuleTransactionPollResult::progress;
    }
    case Phase::commitCheckJournal: {
      StorageIoLease lease = coordinator_->acquireIo(
          owner_, StorageAccess::read, 0);
      if (!lease) return CapsuleTransactionPollResult::wouldBlock;
      if (fs_->exists(journalPath())) {
        journalDurable_ = true;
        phase_ = Phase::recoveryJournalOpen;
      } else {
        targetIndex_ = 0;
        phase_ = Phase::staleStagedExists;
      }
      return CapsuleTransactionPollResult::progress;
    }
    case Phase::staleStagedExists:
    case Phase::staleBackupExists:
    case Phase::staleJournalTemporaryExists: {
      const String path = phase_ == Phase::staleStagedExists
          ? sidePath(targetIndex_, ".new")
          : (phase_ == Phase::staleBackupExists
                 ? sidePath(targetIndex_, ".bak")
                 : journalTemporaryPath());
      StorageIoLease lease = coordinator_->acquireIo(
          owner_, StorageAccess::read, 0);
      if (!lease) return CapsuleTransactionPollResult::wouldBlock;
      const bool exists = fs_->exists(path);
      primitiveFailures_ = 0;
      if (phase_ == Phase::staleStagedExists) {
        phase_ = exists ? Phase::staleStagedRemove :
                          Phase::staleBackupExists;
      } else if (phase_ == Phase::staleBackupExists) {
        phase_ = exists ? Phase::staleBackupRemove : Phase::staleNext;
      } else {
        phase_ = exists ? Phase::staleJournalTemporaryRemove :
                          Phase::prepareTarget;
      }
      return CapsuleTransactionPollResult::progress;
    }
    case Phase::staleStagedRemove:
      return removeIfPresent(sidePath(targetIndex_, ".new"),
                             Phase::staleBackupExists);
    case Phase::staleBackupRemove:
      return removeIfPresent(sidePath(targetIndex_, ".bak"),
                             Phase::staleNext);
    case Phase::staleNext:
      ++targetIndex_;
      if (targetIndex_ < targetCount_) {
        phase_ = Phase::staleStagedExists;
      } else {
        targetIndex_ = 0;
        phase_ = Phase::staleJournalTemporaryExists;
      }
      return CapsuleTransactionPollResult::progress;
    case Phase::staleJournalTemporaryRemove:
      return removeIfPresent(journalTemporaryPath(), Phase::prepareTarget);
    case Phase::prepareTarget: {
      TargetRuntime &target = targets_[targetIndex_];
      target.offset = 0;
      target.crcState = 0xffffffffU;
      phase_ = target.source != nullptr ? Phase::openStage :
                                          Phase::openPreparedFact;
      return CapsuleTransactionPollResult::progress;
    }
    case Phase::openStage: {
      StorageIoLease lease = coordinator_->acquireIo(
          owner_, StorageAccess::mutation, 0);
      if (!lease) return CapsuleTransactionPollResult::wouldBlock;
      file_ = fs_->open(sidePath(targetIndex_, ".new"), FILE_WRITE);
      if (!file_) return primitiveFailed(false);
      fileAccess_ = StorageAccess::mutation;
      primitiveFailures_ = 0;
      phase_ = targets_[targetIndex_].source->length() == 0
          ? Phase::flushStage : Phase::writeStage;
      return CapsuleTransactionPollResult::progress;
    }
    case Phase::writeStage: {
      TargetRuntime &target = targets_[targetIndex_];
      const uint32_t length = target.source->length();
      const uint32_t remaining = length - target.offset;
      const size_t wanted = remaining < kCapsuleTransactionIoBytes
          ? static_cast<size_t>(remaining) : kCapsuleTransactionIoBytes;
      const size_t supplied = target.source->readAt(
          target.offset, ioBuffer_, wanted);
      if (supplied != wanted) {
        beginCleanup(CapsuleTransactionRunState::failed, false);
        return CapsuleTransactionPollResult::progress;
      }
      StorageIoLease lease = coordinator_->acquireIo(
          owner_, StorageAccess::mutation, 0);
      if (!lease) return CapsuleTransactionPollResult::wouldBlock;
      const size_t written = file_.write(ioBuffer_, supplied);
      lastPollBytes_ = written;
      if (written > maximumIoBytes_) maximumIoBytes_ = written;
      if (written != supplied || file_.getWriteError() != 0) {
        beginCleanup(CapsuleTransactionRunState::failed, false);
        return CapsuleTransactionPollResult::progress;
      }
      target.crcState = capsuleTransactionCrc32Update(
          target.crcState, ioBuffer_, written);
      target.offset += static_cast<uint32_t>(written);
      primitiveFailures_ = 0;
      if (target.offset == length) phase_ = Phase::flushStage;
      return CapsuleTransactionPollResult::progress;
    }
    case Phase::flushStage:
    case Phase::flushJournalTemporary: {
      StorageIoLease lease = coordinator_->acquireIo(
          owner_, StorageAccess::mutation, 0);
      if (!lease) return CapsuleTransactionPollResult::wouldBlock;
      file_.flush();
      if (file_.getWriteError() != 0) {
        beginCleanup(journalDurable_ ?
                         CapsuleTransactionRunState::recoveryBlocked :
                         CapsuleTransactionRunState::failed,
                     journalDurable_);
        return CapsuleTransactionPollResult::progress;
      }
      primitiveFailures_ = 0;
      phase_ = phase_ == Phase::flushStage ? Phase::closeStage :
                                            Phase::closeJournalTemporary;
      return CapsuleTransactionPollResult::progress;
    }
    case Phase::closeStage: {
      TargetRuntime &target = targets_[targetIndex_];
      target.expectedLength = target.source->length();
      target.expectedCrc32 = ~target.crcState;
      return closeHandle(Phase::targetExistsForJournal, true);
    }
    case Phase::openPreparedFact:
      return openRead(targets_[targetIndex_].preparedPath, FactKind::source,
                      Phase::cleanupCloseFile, Phase::readFact);
    case Phase::readFact:
      return readFactChunk();
    case Phase::closeFact: {
      const uint32_t crc = ~factCrcState_;
      if (factKind_ == FactKind::source) {
        targets_[targetIndex_].expectedLength = factLength_;
        targets_[targetIndex_].expectedCrc32 = crc;
        return closeHandle(Phase::targetExistsForJournal, false);
      }
      TargetRuntime &target = targets_[targetIndex_];
      if (factKind_ == FactKind::target) {
        target.targetExists = factExists_;
        target.targetMatches = factExists_ &&
            factLength_ == target.expectedLength &&
            crc == target.expectedCrc32;
        return closeHandle(Phase::recoveryStagedFactOpen, false);
      }
      target.stagedExists = factExists_;
      target.stagedMatches = factExists_ &&
          factLength_ == target.expectedLength &&
          crc == target.expectedCrc32;
      return closeHandle(Phase::recoveryBackupExists, false);
    }
    case Phase::targetExistsForJournal: {
      StorageIoLease lease = coordinator_->acquireIo(
          owner_, StorageAccess::read, 0);
      if (!lease) return CapsuleTransactionPollResult::wouldBlock;
      TargetRuntime &target = targets_[targetIndex_];
      target.hadOriginal = fs_->exists(target.targetPath);
      if (!setCapsuleTransactionTarget(
              journal_.targets[targetIndex_], target.targetPath.c_str(),
              target.expectedLength, target.expectedCrc32,
              target.hadOriginal)) {
        beginCleanup(CapsuleTransactionRunState::failed, false);
      } else {
        phase_ = Phase::nextTarget;
      }
      return CapsuleTransactionPollResult::progress;
    }
    case Phase::nextTarget:
      ++targetIndex_;
      if (targetIndex_ < targetCount_) {
        phase_ = Phase::prepareTarget;
      } else {
        journal_.magic = kCapsuleTransactionMagic;
        journal_.version = kCapsuleTransactionVersion;
        journal_.targetCount = targetCount_;
        finalizeCapsuleTransactionJournal(journal_);
        phase_ = Phase::openJournalTemporary;
      }
      return CapsuleTransactionPollResult::progress;
    case Phase::openJournalTemporary: {
      StorageIoLease lease = coordinator_->acquireIo(
          owner_, StorageAccess::mutation, 0);
      if (!lease) return CapsuleTransactionPollResult::wouldBlock;
      file_ = fs_->open(journalTemporaryPath(), FILE_WRITE);
      if (!file_) return primitiveFailed(false);
      fileAccess_ = StorageAccess::mutation;
      primitiveFailures_ = 0;
      phase_ = Phase::writeJournalTemporary;
      return CapsuleTransactionPollResult::progress;
    }
    case Phase::writeJournalTemporary: {
      StorageIoLease lease = coordinator_->acquireIo(
          owner_, StorageAccess::mutation, 0);
      if (!lease) return CapsuleTransactionPollResult::wouldBlock;
      const size_t bytes = file_.write(
          reinterpret_cast<const uint8_t *>(&journal_), sizeof(journal_));
      lastPollBytes_ = bytes;
      if (bytes > maximumIoBytes_) maximumIoBytes_ = bytes;
      if (bytes != sizeof(journal_) || file_.getWriteError() != 0) {
        beginCleanup(CapsuleTransactionRunState::failed, false);
      } else {
        primitiveFailures_ = 0;
        phase_ = Phase::flushJournalTemporary;
      }
      return CapsuleTransactionPollResult::progress;
    }
    case Phase::closeJournalTemporary:
      return closeHandle(Phase::oldJournalExists, true);
    case Phase::oldJournalExists: {
      StorageIoLease lease = coordinator_->acquireIo(
          owner_, StorageAccess::read, 0);
      if (!lease) return CapsuleTransactionPollResult::wouldBlock;
      phase_ = fs_->exists(journalPath()) ? Phase::oldJournalRemove :
                                           Phase::publishJournal;
      return CapsuleTransactionPollResult::progress;
    }
    case Phase::oldJournalRemove:
      return removeIfPresent(journalPath(), Phase::publishJournal);
    case Phase::publishJournal: {
      const CapsuleTransactionPollResult result = renamePath(
          journalTemporaryPath(), journalPath(), Phase::publishPrepared);
      if (phase_ == Phase::publishPrepared) {
        journalDurable_ = true;
        targetIndex_ = 0;
      }
      return result;
    }
    case Phase::publishPrepared:
      if (targetIndex_ >= targetCount_) {
        targetIndex_ = 0;
        phase_ = Phase::recoveryTargetFactOpen;
        return CapsuleTransactionPollResult::progress;
      }
      if (targets_[targetIndex_].preparedPath.isEmpty()) {
        ++targetIndex_;
        return CapsuleTransactionPollResult::progress;
      } else {
        const CapsuleTransactionPollResult result = renamePath(
            targets_[targetIndex_].preparedPath,
            sidePath(targetIndex_, ".new"), Phase::publishPrepared);
        if (result == CapsuleTransactionPollResult::progress &&
            phase_ == Phase::publishPrepared) {
          ++targetIndex_;
        }
        return result;
      }
    case Phase::recoveryScanOpen: {
      StorageIoLease lease = coordinator_->acquireIo(
          owner_, StorageAccess::read, 0);
      if (!lease) return CapsuleTransactionPollResult::wouldBlock;
      directory_ = fs_->open(kTransactionDirectory, FILE_READ);
      if (!directory_ || !directory_.isDirectory()) {
        beginCleanup(CapsuleTransactionRunState::failed, true);
      } else {
        phase_ = Phase::recoveryScanNext;
      }
      return CapsuleTransactionPollResult::progress;
    }
    case Phase::recoveryScanNext: {
      StorageIoLease lease = coordinator_->acquireIo(
          owner_, StorageAccess::read, 0);
      if (!lease) return CapsuleTransactionPollResult::wouldBlock;
      entry_ = directory_.openNextFile();
      if (!entry_) {
        phase_ = Phase::recoveryScanCloseDirectory;
        base_ = "";
        return CapsuleTransactionPollResult::progress;
      }
      String path = entry_.name();
      if (!entry_.isDirectory() && path.endsWith(".journal")) {
        if (!path.startsWith("/")) {
          path = String(kTransactionDirectory) + "/" + path;
        }
        base_ = path.substring(0, path.length() - 8);
      } else {
        base_ = "";
      }
      phase_ = Phase::recoveryScanCloseEntry;
      return CapsuleTransactionPollResult::progress;
    }
    case Phase::recoveryScanCloseEntry: {
      StorageIoLease lease = coordinator_->acquireIo(
          owner_, StorageAccess::read, 0);
      if (!lease) return CapsuleTransactionPollResult::wouldBlock;
      entry_.close();
      phase_ = base_.isEmpty() ? Phase::recoveryScanNext :
                                Phase::recoveryScanCloseDirectory;
      return CapsuleTransactionPollResult::progress;
    }
    case Phase::recoveryScanCloseDirectory: {
      StorageIoLease lease = coordinator_->acquireIo(
          owner_, StorageAccess::read, 0);
      if (!lease) return CapsuleTransactionPollResult::wouldBlock;
      directory_.close();
      if (base_.isEmpty()) {
        return finishTerminal(CapsuleTransactionRunState::recovered);
      }
      journalDurable_ = true;
      phase_ = Phase::recoveryJournalOpen;
      return CapsuleTransactionPollResult::progress;
    }
    case Phase::recoveryJournalOpen: {
      StorageIoLease lease = coordinator_->acquireIo(
          owner_, StorageAccess::read, 0);
      if (!lease) return CapsuleTransactionPollResult::wouldBlock;
      file_ = fs_->open(journalPath(), FILE_READ);
      if (!file_ || file_.isDirectory() ||
          file_.size() != sizeof(journal_)) {
        beginCleanup(CapsuleTransactionRunState::recoveryBlocked, true);
      } else {
        fileAccess_ = StorageAccess::read;
        phase_ = Phase::recoveryJournalRead;
      }
      return CapsuleTransactionPollResult::progress;
    }
    case Phase::recoveryJournalRead: {
      StorageIoLease lease = coordinator_->acquireIo(
          owner_, StorageAccess::read, 0);
      if (!lease) return CapsuleTransactionPollResult::wouldBlock;
      const int bytes = file_.read(
          reinterpret_cast<uint8_t *>(&journal_), sizeof(journal_));
      lastPollBytes_ = bytes > 0 ? static_cast<size_t>(bytes) : 0;
      if (lastPollBytes_ > maximumIoBytes_) maximumIoBytes_ = lastPollBytes_;
      if (bytes != static_cast<int>(sizeof(journal_)) ||
          !validateCapsuleTransactionJournal(journal_)) {
        beginCleanup(CapsuleTransactionRunState::recoveryBlocked, true);
      } else {
        targetCount_ = journal_.targetCount;
        for (uint8_t index = 0; index < targetCount_; ++index) {
          targets_[index] = {};
          targets_[index].targetPath = journal_.targets[index].path;
          targets_[index].expectedLength =
              journal_.targets[index].expectedLength;
          targets_[index].expectedCrc32 =
              journal_.targets[index].expectedCrc32;
          targets_[index].hadOriginal =
              journal_.targets[index].hadOriginal != 0;
        }
        phase_ = Phase::recoveryJournalClose;
      }
      return CapsuleTransactionPollResult::progress;
    }
    case Phase::recoveryJournalClose:
      targetIndex_ = 0;
      return closeHandle(Phase::recoveryTargetFactOpen, false);
    case Phase::recoveryTargetFactOpen:
      return openRead(targets_[targetIndex_].targetPath, FactKind::target,
                      Phase::recoveryStagedFactOpen, Phase::readFact);
    case Phase::recoveryStagedFactOpen:
      return openRead(sidePath(targetIndex_, ".new"), FactKind::staged,
                      Phase::recoveryBackupExists, Phase::readFact);
    case Phase::recoveryBackupExists: {
      StorageIoLease lease = coordinator_->acquireIo(
          owner_, StorageAccess::read, 0);
      if (!lease) return CapsuleTransactionPollResult::wouldBlock;
      targets_[targetIndex_].backupExists =
          fs_->exists(sidePath(targetIndex_, ".bak"));
      phase_ = Phase::recoveryFactsNext;
      return CapsuleTransactionPollResult::progress;
    }
    case Phase::recoveryFactsNext:
      ++targetIndex_;
      if (targetIndex_ < targetCount_) {
        phase_ = Phase::recoveryTargetFactOpen;
      } else {
        phase_ = Phase::recoveryDecide;
      }
      return CapsuleTransactionPollResult::progress;
    case Phase::recoveryDecide: {
      CapsuleTransactionTargetFacts facts[
          kCapsuleTransactionMaximumTargets]{};
      for (uint8_t index = 0; index < targetCount_; ++index) {
        const TargetRuntime &target = targets_[index];
        facts[index] = {target.hadOriginal, target.targetExists,
                        target.targetMatches,
                        target.stagedExists && target.stagedMatches,
                        target.backupExists};
      }
      recoveryDecision_ = decideCapsuleTransactionRecovery(
          facts, targetCount_);
      if (recoveryDecision_ == CapsuleTransactionRecovery::ambiguous) {
        if (log_ != nullptr) {
          log_->printf(
              "{\"event\":\"capsule_transaction_ambiguous\","
              "\"journal\":\"%s\"}\n", journalPath().c_str());
        }
        beginCleanup(CapsuleTransactionRunState::recoveryBlocked, true);
      } else {
        targetIndex_ = 0;
        if (recoveryDecision_ == CapsuleTransactionRecovery::commitNew) {
          phase_ = Phase::commitTarget;
        } else if (recoveryDecision_ ==
                   CapsuleTransactionRecovery::rollbackOld) {
          phase_ = Phase::rollbackTarget;
        } else {
          cleanupTerminal_ = mode_ == Mode::commit
              ? CapsuleTransactionRunState::committed
              : CapsuleTransactionRunState::recovered;
          preserveJournal_ = false;
          cleanupIndex_ = 0;
          phase_ = Phase::cleanupStagedExists;
        }
      }
      return CapsuleTransactionPollResult::progress;
    }
    case Phase::commitTarget: {
      TargetRuntime &target = targets_[targetIndex_];
      if (target.targetMatches) {
        phase_ = Phase::commitNext;
      } else if (target.targetExists) {
        phase_ = target.backupExists ? Phase::commitBackupRemove :
                                      Phase::commitTargetRename;
      } else {
        phase_ = Phase::commitStagedRename;
      }
      return CapsuleTransactionPollResult::progress;
    }
    case Phase::commitBackupRemove: {
      const CapsuleTransactionPollResult result = removeIfPresent(
          sidePath(targetIndex_, ".bak"), Phase::commitTargetRename);
      if (phase_ == Phase::commitTargetRename) {
        targets_[targetIndex_].backupExists = false;
      }
      return result;
    }
    case Phase::commitTargetRename: {
      const CapsuleTransactionPollResult result = renamePath(
          targets_[targetIndex_].targetPath,
          sidePath(targetIndex_, ".bak"), Phase::commitStagedRename);
      if (phase_ == Phase::commitStagedRename) {
        targets_[targetIndex_].targetExists = false;
        targets_[targetIndex_].backupExists = true;
      }
      return result;
    }
    case Phase::commitStagedRename: {
      const CapsuleTransactionPollResult result = renamePath(
          sidePath(targetIndex_, ".new"),
          targets_[targetIndex_].targetPath, Phase::commitNext);
      if (phase_ == Phase::commitNext) {
        targets_[targetIndex_].targetExists = true;
        targets_[targetIndex_].targetMatches = true;
        targets_[targetIndex_].stagedExists = false;
      }
      return result;
    }
    case Phase::commitNext:
      ++targetIndex_;
      if (targetIndex_ < targetCount_) {
        phase_ = Phase::commitTarget;
      } else {
        cleanupTerminal_ = mode_ == Mode::commit
            ? CapsuleTransactionRunState::committed
            : CapsuleTransactionRunState::recovered;
        cleanupIndex_ = 0;
        preserveJournal_ = false;
        phase_ = Phase::cleanupStagedExists;
      }
      return CapsuleTransactionPollResult::progress;
    case Phase::rollbackTarget: {
      TargetRuntime &target = targets_[targetIndex_];
      if (target.hadOriginal && target.backupExists) {
        phase_ = target.targetExists ? Phase::rollbackTargetRemove :
                                      Phase::rollbackBackupRename;
      } else if (!target.hadOriginal && target.targetExists &&
                 target.targetMatches) {
        phase_ = Phase::rollbackNewRemove;
      } else {
        phase_ = Phase::rollbackNext;
      }
      return CapsuleTransactionPollResult::progress;
    }
    case Phase::rollbackTargetRemove:
      return removeIfPresent(targets_[targetIndex_].targetPath,
                             Phase::rollbackBackupRename);
    case Phase::rollbackBackupRename:
      return renamePath(sidePath(targetIndex_, ".bak"),
                        targets_[targetIndex_].targetPath,
                        Phase::rollbackNext);
    case Phase::rollbackNewRemove:
      return removeIfPresent(targets_[targetIndex_].targetPath,
                             Phase::rollbackNext);
    case Phase::rollbackNext:
      ++targetIndex_;
      if (targetIndex_ < targetCount_) {
        phase_ = Phase::rollbackTarget;
      } else {
        if (cleanupTerminal_ != CapsuleTransactionRunState::cancelled) {
          cleanupTerminal_ = mode_ == Mode::commit
              ? CapsuleTransactionRunState::committed
              : CapsuleTransactionRunState::recovered;
        }
        cleanupIndex_ = 0;
        preserveJournal_ = false;
        phase_ = Phase::cleanupStagedExists;
      }
      return CapsuleTransactionPollResult::progress;
    case Phase::cleanupCloseFile: {
      StorageIoLease lease = coordinator_->acquireIo(
          owner_, fileAccess_, 0);
      if (!lease) return CapsuleTransactionPollResult::wouldBlock;
      if (file_) file_.close();
      fileAccess_ = StorageAccess::read;
      phase_ = entry_ ? Phase::cleanupCloseEntry :
          (directory_ ? Phase::cleanupCloseDirectory :
           (preserveJournal_ ? Phase::cleanupDone :
                               Phase::cleanupStagedExists));
      return CapsuleTransactionPollResult::progress;
    }
    case Phase::cleanupCloseEntry: {
      StorageIoLease lease = coordinator_->acquireIo(
          owner_, StorageAccess::read, 0);
      if (!lease) return CapsuleTransactionPollResult::wouldBlock;
      if (entry_) entry_.close();
      phase_ = directory_ ? Phase::cleanupCloseDirectory :
          (preserveJournal_ ? Phase::cleanupDone :
                              Phase::cleanupStagedExists);
      return CapsuleTransactionPollResult::progress;
    }
    case Phase::cleanupCloseDirectory: {
      StorageIoLease lease = coordinator_->acquireIo(
          owner_, StorageAccess::read, 0);
      if (!lease) return CapsuleTransactionPollResult::wouldBlock;
      if (directory_) directory_.close();
      phase_ = preserveJournal_ ? Phase::cleanupDone :
                                  Phase::cleanupStagedExists;
      return CapsuleTransactionPollResult::progress;
    }
    case Phase::cleanupStagedExists:
    case Phase::cleanupBackupExists:
    case Phase::cleanupJournalTemporaryExists:
    case Phase::cleanupJournalExists: {
      const String path = phase_ == Phase::cleanupStagedExists
          ? sidePath(cleanupIndex_, ".new")
          : (phase_ == Phase::cleanupBackupExists
                 ? sidePath(cleanupIndex_, ".bak")
                 : (phase_ == Phase::cleanupJournalTemporaryExists
                        ? journalTemporaryPath() : journalPath()));
      StorageIoLease lease = coordinator_->acquireIo(
          owner_, StorageAccess::read, 0);
      if (!lease) return CapsuleTransactionPollResult::wouldBlock;
      cleanupPathExists_ = fs_->exists(path);
      primitiveFailures_ = 0;
      if (phase_ == Phase::cleanupStagedExists) {
        phase_ = cleanupPathExists_ ? Phase::cleanupStagedRemove :
                                     Phase::cleanupBackupExists;
      } else if (phase_ == Phase::cleanupBackupExists) {
        phase_ = cleanupPathExists_ ? Phase::cleanupBackupRemove :
                                     Phase::cleanupNext;
      } else if (phase_ == Phase::cleanupJournalTemporaryExists) {
        phase_ = cleanupPathExists_ ?
            Phase::cleanupJournalTemporaryRemove :
            Phase::cleanupJournalExists;
      } else {
        phase_ = cleanupPathExists_ ? Phase::cleanupJournalRemove :
                                     Phase::cleanupDone;
      }
      return CapsuleTransactionPollResult::progress;
    }
    case Phase::cleanupStagedRemove:
      return removeIfPresent(sidePath(cleanupIndex_, ".new"),
                             Phase::cleanupBackupExists);
    case Phase::cleanupBackupRemove:
      return removeIfPresent(sidePath(cleanupIndex_, ".bak"),
                             Phase::cleanupNext);
    case Phase::cleanupNext:
      ++cleanupIndex_;
      if (cleanupIndex_ < targetCount_) {
        phase_ = Phase::cleanupStagedExists;
      } else {
        phase_ = Phase::cleanupJournalTemporaryExists;
      }
      return CapsuleTransactionPollResult::progress;
    case Phase::cleanupJournalTemporaryRemove:
      return removeIfPresent(journalTemporaryPath(),
                             Phase::cleanupJournalExists);
    case Phase::cleanupJournalRemove:
      return removeIfPresent(journalPath(), Phase::cleanupDone);
    case Phase::cleanupDone:
      journalDurable_ = false;
      if (mode_ == Mode::recoverAll &&
          cleanupTerminal_ == CapsuleTransactionRunState::recovered &&
          !cancellationRequested_) {
        base_ = "";
        targetCount_ = 0;
        targetIndex_ = 0;
        phase_ = Phase::recoveryScanOpen;
        return CapsuleTransactionPollResult::progress;
      }
      return finishTerminal(cleanupTerminal_);
  }
  beginCleanup(CapsuleTransactionRunState::failed, journalDurable_);
  return CapsuleTransactionPollResult::progress;
}

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
