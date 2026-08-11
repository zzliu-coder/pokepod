#pragma once

#include <Arduino.h>
#include <FS.h>

#include "CapsuleTransactionPolicy.h"
#include "StorageCoordinator.h"

namespace pokepod {

constexpr size_t kCapsuleTransactionPollBytes = 16U * 1024U;
constexpr size_t kCapsuleTransactionIoBytes = 4U * 1024U;

class CapsuleTransactionGate {
 public:
  virtual ~CapsuleTransactionGate() = default;
  virtual bool permits(uint32_t nowMs) = 0;
};

inline bool capsuleTransactionPermitted(CapsuleTransactionGate *gate,
                                        uint32_t nowMs) {
  return gate == nullptr || gate->permits(nowMs);
}

// A wrap-safe absolute deadline. The connection/window owner arms it once;
// transaction activity never arms, extends, or otherwise owns the deadline.
// Callers without a deadline (USB) pass a null gate to poll().
class AbsoluteCapsuleTransactionDeadlineGate final
    : public CapsuleTransactionGate {
 public:
  void arm(uint32_t nowMs, uint32_t durationMs) {
    deadlineMs_ = nowMs + durationMs;
    active_ = true;
  }
  void cancel() {
    active_ = false;
    deadlineMs_ = 0;
  }
  bool permits(uint32_t nowMs) override {
    return active_ && static_cast<int32_t>(nowMs - deadlineMs_) < 0;
  }
  bool active() const { return active_; }
  uint32_t deadlineMs() const { return deadlineMs_; }

 private:
  bool active_ = false;
  uint32_t deadlineMs_ = 0;
};

class CapsuleTransactionByteSource {
 public:
  virtual ~CapsuleTransactionByteSource() = default;
  virtual uint32_t length() const = 0;
  virtual size_t readAt(uint32_t offset, uint8_t *destination,
                        size_t maximumBytes) = 0;
};

struct CapsuleTransactionInput {
  String targetPath;
  CapsuleTransactionByteSource *source = nullptr;
  String preparedPath;
};

enum class CapsuleTransactionRunState : uint8_t {
  idle = 0,
  running,
  committed,
  recovered,
  cancelled,
  failed,
  cleanupBlocked,
  recoveryBlocked,
};

enum class CapsuleTransactionPollResult : uint8_t {
  idle = 0,
  progress,
  wouldBlock,
  committed,
  recovered,
  cancelled,
  failed,
  cleanupBlocked,
  recoveryBlocked,
};

// Persistent, bounded production transaction engine. Every poll performs at
// most one <=4 KiB read/write or one atomic filesystem primitive. Open File
// handles and the logical reservation survive would-block polls. A permanent
// cleanup fault closes handles under lease, preserves restart artifacts, then
// releases the reservation at its deterministic terminal boundary.
class CapsuleTransactionRunner {
 public:
  CapsuleTransactionRunner() = default;
  CapsuleTransactionRunner(const CapsuleTransactionRunner &) = delete;
  CapsuleTransactionRunner &operator=(const CapsuleTransactionRunner &) = delete;

  bool begin(fs::FS &fs, Print &log,
             StorageCoordinator &coordinator = StorageCoordinator::instance());
  // Each non-null source is borrowed and must outlive the operation. File
  // input belongs in preparedPath so all physical reads stay under IO leases.
  bool startCommit(const char *key, const CapsuleTransactionInput *targets,
                   uint8_t count, StorageOwner owner);
  bool startPreparedFile(const char *key, const String &preparedPath,
                         const String &targetPath, StorageOwner owner);
  bool startRecovery(StorageOwner owner = StorageOwner::recovery);
  CapsuleTransactionPollResult poll(uint32_t nowMs,
                                    CapsuleTransactionGate *gate = nullptr);

  CapsuleTransactionRunState state() const { return state_; }
  bool active() const { return state_ == CapsuleTransactionRunState::running; }
  bool terminal() const { return state_ != CapsuleTransactionRunState::idle &&
                                state_ != CapsuleTransactionRunState::running; }
  bool reservationHeld() const {
    return static_cast<bool>(reservation_);
  }
  size_t lastPollBytes() const { return lastPollBytes_; }
  size_t maximumPollBytes() const { return maximumPollBytes_; }
  size_t maximumIoBytes() const { return maximumIoBytes_; }
  uint32_t polls() const { return polls_; }
  const char *phaseName() const;

 private:
  enum class Mode : uint8_t { none = 0, commit, recoverAll };
  enum class Phase : uint8_t;
  enum class FactKind : uint8_t { none = 0, source, target, staged, backup };

  struct TargetRuntime {
    String targetPath;
    CapsuleTransactionByteSource *source = nullptr;
    String preparedPath;
    uint32_t expectedLength = 0;
    uint32_t expectedCrc32 = 0;
    uint32_t offset = 0;
    uint32_t crcState = 0xffffffffU;
    bool hadOriginal = false;
    bool targetExists = false;
    bool targetMatches = false;
    bool stagedExists = false;
    bool stagedMatches = false;
    bool backupExists = false;
  };

  bool startCommon(StorageOwner owner, Mode mode);
  CapsuleTransactionPollResult step();
  CapsuleTransactionPollResult primitiveFailed(bool cleanup);
  CapsuleTransactionPollResult closeHandle(Phase next, bool writing);
  CapsuleTransactionPollResult openRead(const String &path, FactKind kind,
                                        Phase missing, Phase opened);
  CapsuleTransactionPollResult readFactChunk();
  CapsuleTransactionPollResult removeIfPresent(const String &path,
                                               Phase next);
  CapsuleTransactionPollResult renamePath(const String &from,
                                          const String &to, Phase next);
  CapsuleTransactionPollResult finishTerminal(
      CapsuleTransactionRunState state);
  CapsuleTransactionPollResult resultForState() const;
  void beginCleanup(CapsuleTransactionRunState terminalState,
                    bool preserveJournal);
  void resetOperationFields();
  String transactionBase(const char *key) const;
  String sidePath(uint8_t index, const char *suffix) const;
  String journalPath() const { return base_ + ".journal"; }
  String journalTemporaryPath() const { return base_ + ".journal.tmp"; }
  static bool validInput(const CapsuleTransactionInput &input);

  fs::FS *fs_ = nullptr;
  Print *log_ = nullptr;
  StorageCoordinator *coordinator_ = nullptr;
  StorageReservation reservation_;
  File file_;
  File directory_;
  File entry_;
  Mode mode_ = Mode::none;
  Phase phase_ = static_cast<Phase>(0);
  CapsuleTransactionRunState state_ = CapsuleTransactionRunState::idle;
  CapsuleTransactionRunState cleanupTerminal_ =
      CapsuleTransactionRunState::failed;
  StorageOwner owner_ = StorageOwner::none;
  TargetRuntime targets_[kCapsuleTransactionMaximumTargets];
  uint8_t targetCount_ = 0;
  uint8_t targetIndex_ = 0;
  uint8_t cleanupIndex_ = 0;
  uint8_t primitiveFailures_ = 0;
  FactKind factKind_ = FactKind::none;
  StorageAccess fileAccess_ = StorageAccess::read;
  StoredCapsuleTransactionJournal journal_{};
  CapsuleTransactionRecovery recoveryDecision_ =
      CapsuleTransactionRecovery::ambiguous;
  String key_;
  String base_;
  String factPath_;
  uint32_t factLength_ = 0;
  uint32_t factOffset_ = 0;
  uint32_t factCrcState_ = 0xffffffffU;
  bool factExists_ = false;
  bool journalDurable_ = false;
  bool cancellationRequested_ = false;
  bool preserveJournal_ = false;
  bool cleanupPathExists_ = false;
  size_t lastPollBytes_ = 0;
  size_t maximumPollBytes_ = 0;
  size_t maximumIoBytes_ = 0;
  uint32_t polls_ = 0;
  alignas(4) uint8_t ioBuffer_[kCapsuleTransactionIoBytes]{};
};

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
