#pragma once

#include <Arduino.h>
#include <FS.h>
#include <vector>

#include "StorageCoordinator.h"
#include "CapsuleTransactionPolicy.h"

namespace pokepod {

// Bounded recursive copy/removal used by Link command apply, rollback, and
// durable cleanup.  One poll performs at most one directory operation or one
// 4 KiB file transfer.  File handles remain owned by the stepper when physical
// IO is busy; no File destructor is allowed to close outside an owner lease.
class LinkTreeStepper {
 public:
  enum class Mode : uint8_t { none, copy, remove, verifyAbsent };
  enum class Result : uint8_t {
    idle,
    progress,
    wouldBlock,
    complete,
    cancelled,
    failed,
  };

  using Permit = bool (*)(void *context);

  bool beginCopy(fs::FS &fs, const String &source, const String &target,
                 StorageOwner owner, Permit permit = nullptr,
                 void *permitContext = nullptr);
  bool beginRemove(fs::FS &fs, const String &path, StorageOwner owner,
                   Permit permit = nullptr, void *permitContext = nullptr);
  bool beginVerifyAbsent(fs::FS &fs, const String &path,
                         const String &forbiddenLeaf, StorageOwner owner,
                         Permit permit = nullptr,
                         void *permitContext = nullptr);
  Result poll();
  bool active() const { return mode_ != Mode::none; }
  Result result() const { return result_; }
  const String &failedPath() const { return failedPath_; }

  static constexpr size_t kBytesPerPoll = 4096;
  static constexpr uint8_t kMaximumDepth = 6;

 private:
  enum class EntryPhase : uint8_t { inspect, enumerate, finish };
  enum class FileCopyPhase : uint8_t {
    copy, flushOutput, closeOutput, closeInput, openVerify, verify, closeVerify
  };
  struct Entry {
    String source;
    String target;
    File directory;
    EntryPhase phase = EntryPhase::inspect;
  };

  bool permitted() const {
    return permit_ == nullptr || permit_(permitContext_);
  }
  Result fail(const String &path, Result result = Result::failed);
  Result pollCopy();
  Result pollRemove();
  Result pollVerifyAbsent();
  Result pollFileCopy();
  Result closeFileCopy(bool successful);
  Result pollAbort();
  void finish();

  fs::FS *fs_ = nullptr;
  StorageOwner owner_ = StorageOwner::none;
  Permit permit_ = nullptr;
  void *permitContext_ = nullptr;
  Mode mode_ = Mode::none;
  Result result_ = Result::idle;
  std::vector<Entry> stack_;
  File input_;
  File output_;
  String fileSource_;
  String fileTarget_;
  bool fileCopyActive_ = false;
  FileCopyPhase fileCopyPhase_ = FileCopyPhase::copy;
  uint32_t sourceCrcState_ = 0xffffffffU;
  uint32_t targetCrcState_ = 0xffffffffU;
  uint32_t sourceLength_ = 0;
  uint32_t targetLength_ = 0;
  bool abortPending_ = false;
  String failedPath_;
  String forbiddenLeaf_;
  uint8_t buffer_[kBytesPerPoll] = {};
};

}  // namespace pokepod
