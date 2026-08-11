#include "LinkTreeStepper.h"

namespace pokepod {

namespace {

String leafName(const String &path) {
  const int slash = path.lastIndexOf('/');
  return slash < 0 ? path : path.substring(slash + 1);
}

}  // namespace

bool LinkTreeStepper::beginCopy(fs::FS &fs, const String &source,
                                const String &target, StorageOwner owner,
                                Permit permit, void *permitContext) {
  if (active() || source.isEmpty() || target.isEmpty() ||
      owner == StorageOwner::none) return false;
  fs_ = &fs;
  owner_ = owner;
  permit_ = permit;
  permitContext_ = permitContext;
  mode_ = Mode::copy;
  result_ = Result::progress;
  failedPath_ = "";
  stack_.push_back({source, target, File(), EntryPhase::inspect});
  return true;
}

bool LinkTreeStepper::beginRemove(fs::FS &fs, const String &path,
                                  StorageOwner owner, Permit permit,
                                  void *permitContext) {
  if (active() || path.isEmpty() || owner == StorageOwner::none) return false;
  fs_ = &fs;
  owner_ = owner;
  permit_ = permit;
  permitContext_ = permitContext;
  mode_ = Mode::remove;
  result_ = Result::progress;
  failedPath_ = "";
  stack_.push_back({path, String(), File(), EntryPhase::inspect});
  return true;
}

LinkTreeStepper::Result LinkTreeStepper::poll() {
  if (!active()) return result_;
  if (!permitted()) {
    failedPath_ = stack_.empty() ? fileSource_ : stack_.back().source;
    abortPending_ = true;
    permit_ = nullptr;
    permitContext_ = nullptr;
  }
  if (abortPending_) return pollAbort();
  return mode_ == Mode::copy ? pollCopy() : pollRemove();
}

LinkTreeStepper::Result LinkTreeStepper::pollAbort() {
  StorageIoLease lease = StorageCoordinator::instance().acquireIo(
      owner_, StorageAccess::mutation, 0);
  if (!lease) return Result::wouldBlock;
  if (input_) input_.close();
  if (output_) output_.close();
  for (Entry &entry : stack_) {
    if (entry.directory) entry.directory.close();
  }
  stack_.clear();
  fileCopyActive_ = false;
  abortPending_ = false;
  mode_ = Mode::none;
  result_ = Result::cancelled;
  return result_;
}

LinkTreeStepper::Result LinkTreeStepper::pollCopy() {
  if (fileCopyActive_) return pollFileCopy();
  if (stack_.empty()) {
    finish();
    return result_;
  }
  Entry &entry = stack_.back();
  StorageIoLease lease = StorageCoordinator::instance().acquireIo(
      owner_, StorageAccess::mutation, 0);
  if (!lease) return Result::wouldBlock;

  if (entry.phase == EntryPhase::inspect) {
    File source = fs_->open(entry.source, FILE_READ);
    if (!source) return fail(entry.source);
    if (!source.isDirectory()) {
      File target = fs_->open(entry.target, FILE_WRITE);
      if (!target) {
        source.close();
        return fail(entry.target);
      }
      input_ = source;
      output_ = target;
      fileSource_ = entry.source;
      fileTarget_ = entry.target;
      fileCopyActive_ = true;
      stack_.pop_back();
      return Result::progress;
    }
    if (!fs_->exists(entry.target) && !fs_->mkdir(entry.target)) {
      source.close();
      return fail(entry.target);
    }
    entry.directory = source;
    entry.phase = EntryPhase::enumerate;
    return Result::progress;
  }

  if (entry.phase == EntryPhase::enumerate) {
    File child = entry.directory.openNextFile();
    if (!child) {
      entry.phase = EntryPhase::finish;
      return Result::progress;
    }
    const String full = child.name();
    child.close();
    if (stack_.size() >= kMaximumDepth + 1) return fail(full);
    const String name = leafName(full);
    stack_.push_back({entry.source + "/" + name,
                      entry.target + "/" + name, File(),
                      EntryPhase::inspect});
    return Result::progress;
  }

  if (entry.directory) entry.directory.close();
  stack_.pop_back();
  return Result::progress;
}

LinkTreeStepper::Result LinkTreeStepper::pollFileCopy() {
  StorageIoLease lease = StorageCoordinator::instance().acquireIo(
      owner_, StorageAccess::mutation, 0);
  if (!lease) return Result::wouldBlock;
  if (!input_ || !output_) return closeFileCopy(false);
  if (!input_.available()) return closeFileCopy(true);
  const size_t count = input_.read(buffer_, sizeof(buffer_));
  if (count == 0 || output_.write(buffer_, count) != count) {
    return closeFileCopy(false);
  }
  return Result::progress;
}

LinkTreeStepper::Result LinkTreeStepper::closeFileCopy(bool successful) {
  // Caller holds the physical mutation lease for every invocation.
  if (output_) {
    output_.flush();
    successful = successful && output_.getWriteError() == 0;
    output_.close();
  }
  if (input_) input_.close();
  fileCopyActive_ = false;
  if (!successful) return fail(fileTarget_);
  fileSource_ = fileTarget_ = "";
  return Result::progress;
}

LinkTreeStepper::Result LinkTreeStepper::pollRemove() {
  if (stack_.empty()) {
    finish();
    return result_;
  }
  Entry &entry = stack_.back();
  StorageIoLease lease = StorageCoordinator::instance().acquireIo(
      owner_, StorageAccess::mutation, 0);
  if (!lease) return Result::wouldBlock;

  if (entry.phase == EntryPhase::inspect) {
    File current = fs_->open(entry.source);
    if (!current) {
      stack_.pop_back();
      return Result::progress;
    }
    if (!current.isDirectory()) {
      current.close();
      if (!fs_->remove(entry.source)) return fail(entry.source);
      stack_.pop_back();
      return Result::progress;
    }
    entry.directory = current;
    entry.phase = EntryPhase::enumerate;
    return Result::progress;
  }

  if (entry.phase == EntryPhase::enumerate) {
    File child = entry.directory.openNextFile();
    if (!child) {
      entry.phase = EntryPhase::finish;
      return Result::progress;
    }
    const String full = child.name();
    child.close();
    if (stack_.size() >= kMaximumDepth + 1) return fail(full);
    stack_.push_back({entry.source + "/" + leafName(full), String(), File(),
                      EntryPhase::inspect});
    return Result::progress;
  }

  if (entry.directory) entry.directory.close();
  if (!fs_->rmdir(entry.source)) return fail(entry.source);
  stack_.pop_back();
  return Result::progress;
}

LinkTreeStepper::Result LinkTreeStepper::fail(const String &path,
                                              Result result) {
  failedPath_ = path;
  // All currently open handles are closed while the caller still owns the
  // physical lease. Cancellation uses pollAbort() so an unavailable lease
  // retains all handles for the next local cleanup poll.
  if (input_) input_.close();
  if (output_) output_.close();
  for (Entry &entry : stack_) {
    if (entry.directory) entry.directory.close();
  }
  stack_.clear();
  fileCopyActive_ = false;
  abortPending_ = false;
  mode_ = Mode::none;
  result_ = result;
  return result_;
}

void LinkTreeStepper::finish() {
  mode_ = Mode::none;
  result_ = Result::complete;
  fs_ = nullptr;
  owner_ = StorageOwner::none;
  permit_ = nullptr;
  permitContext_ = nullptr;
  failedPath_ = "";
}

}  // namespace pokepod
