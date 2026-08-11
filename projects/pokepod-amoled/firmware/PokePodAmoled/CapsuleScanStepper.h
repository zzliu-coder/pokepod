#pragma once

#include <stddef.h>
#include <stdint.h>

namespace pokepod {

// The scan service deliberately budgets logical filesystem work instead of
// wall time. SD latency is unbounded, while entry and byte budgets guarantee
// that one invocation never turns back into an unbounded directory walk.
struct CapsuleScanBudget {
  size_t directoryEntries = 1;
  size_t readBytes = 1024;
};

enum class CapsuleScanState : uint8_t {
  idle = 0,
  running,
  completed,
  cancelled,
  failed,
};

struct CapsuleScanSliceStats {
  size_t directoryEntries = 0;
  size_t readBytes = 0;
};

class CapsuleScanStepper {
 public:
  void begin(const CapsuleScanBudget &budget = {}) {
    budget_ = budget;
    if (budget_.directoryEntries == 0) budget_.directoryEntries = 1;
    if (budget_.readBytes == 0) budget_.readBytes = 512;
    state_ = CapsuleScanState::running;
    current_ = {};
    slices_ = 0;
    totalDirectoryEntries_ = 0;
    totalReadBytes_ = 0;
    maximumDirectoryEntries_ = 0;
    maximumReadBytes_ = 0;
  }

  bool startSlice() {
    if (state_ != CapsuleScanState::running) return false;
    current_ = {};
    return true;
  }

  size_t remainingDirectoryEntries() const {
    return current_.directoryEntries >= budget_.directoryEntries
        ? 0 : budget_.directoryEntries - current_.directoryEntries;
  }

  size_t remainingReadBytes() const {
    return current_.readBytes >= budget_.readBytes
        ? 0 : budget_.readBytes - current_.readBytes;
  }

  bool consumeDirectoryEntry() {
    if (state_ != CapsuleScanState::running ||
        remainingDirectoryEntries() == 0) return false;
    ++current_.directoryEntries;
    ++totalDirectoryEntries_;
    return true;
  }

  size_t consumeReadBytes(size_t requested) {
    if (state_ != CapsuleScanState::running) return 0;
    const size_t accepted = requested < remainingReadBytes()
        ? requested : remainingReadBytes();
    current_.readBytes += accepted;
    totalReadBytes_ += accepted;
    return accepted;
  }

  void finishSlice() {
    if (state_ != CapsuleScanState::running) return;
    ++slices_;
    if (current_.directoryEntries > maximumDirectoryEntries_) {
      maximumDirectoryEntries_ = current_.directoryEntries;
    }
    if (current_.readBytes > maximumReadBytes_) {
      maximumReadBytes_ = current_.readBytes;
    }
  }

  void complete() { transition(CapsuleScanState::completed); }
  void cancel() { transition(CapsuleScanState::cancelled); }
  void fail() { transition(CapsuleScanState::failed); }

  CapsuleScanState state() const { return state_; }
  bool active() const { return state_ == CapsuleScanState::running; }
  const CapsuleScanBudget &budget() const { return budget_; }
  const CapsuleScanSliceStats &currentSlice() const { return current_; }
  uint32_t slices() const { return slices_; }
  size_t totalDirectoryEntries() const { return totalDirectoryEntries_; }
  size_t totalReadBytes() const { return totalReadBytes_; }
  size_t maximumDirectoryEntries() const { return maximumDirectoryEntries_; }
  size_t maximumReadBytes() const { return maximumReadBytes_; }

 private:
  void transition(CapsuleScanState state) {
    if (state_ != CapsuleScanState::running) return;
    state_ = state;
  }

  CapsuleScanBudget budget_{};
  CapsuleScanState state_ = CapsuleScanState::idle;
  CapsuleScanSliceStats current_{};
  uint32_t slices_ = 0;
  size_t totalDirectoryEntries_ = 0;
  size_t totalReadBytes_ = 0;
  size_t maximumDirectoryEntries_ = 0;
  size_t maximumReadBytes_ = 0;
};

}  // namespace pokepod
