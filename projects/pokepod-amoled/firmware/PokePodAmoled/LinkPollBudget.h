#pragma once

#include <stddef.h>
#include <stdint.h>

namespace pokepod {

using LinkPollClock = uint64_t (*)(void *context);

// A poll slice has two independent ceilings.  Byte accounting bounds work
// when the clock is coarse or mocked; elapsed monotonic time bounds work when
// a transport byte unexpectedly becomes expensive.  Exhaustion only yields
// to the caller: parser/request state remains owned by the Link service.
class LinkPollBudget {
 public:
  LinkPollBudget(size_t byteLimit, uint64_t timeLimitUs, uint64_t startedUs)
      : byteLimit_(byteLimit), timeLimitUs_(timeLimitUs),
        startedUs_(startedUs) {}

  bool permits(uint64_t nowUs) const {
    return bytesUsed_ < byteLimit_ && nowUs >= startedUs_ &&
        nowUs - startedUs_ < timeLimitUs_;
  }

  void consume(size_t bytes = 1) {
    const size_t remaining = byteLimit_ -
        (bytesUsed_ < byteLimit_ ? bytesUsed_ : byteLimit_);
    bytesUsed_ += bytes > remaining ? remaining : bytes;
  }

  size_t bytesUsed() const { return bytesUsed_; }
  size_t bytesRemaining() const {
    return bytesUsed_ < byteLimit_ ? byteLimit_ - bytesUsed_ : 0;
  }

 private:
  size_t byteLimit_ = 0;
  uint64_t timeLimitUs_ = 0;
  uint64_t startedUs_ = 0;
  size_t bytesUsed_ = 0;
};

// Every production poll phase goes through one gate backed by the same
// budget.  The injected clock keeps the scheduling rule host-testable without
// weakening the ESP32 monotonic-clock contract.
class LinkPollPhaseGate {
 public:
  LinkPollPhaseGate(LinkPollBudget &budget, LinkPollClock clock,
                    void *clockContext = nullptr)
      : budget_(budget), clock_(clock), clockContext_(clockContext) {}

  bool checkpoint() const {
    return clock_ != nullptr && budget_.permits(clock_(clockContext_));
  }

  template <typename Phase>
  bool run(Phase phase) {
    if (!checkpoint()) return false;
    phase();
    return checkpoint();
  }

  void consumeBytes(size_t bytes = 1) { budget_.consume(bytes); }
  size_t bytesRemaining() const { return budget_.bytesRemaining(); }

 private:
  LinkPollBudget &budget_;
  LinkPollClock clock_ = nullptr;
  void *clockContext_ = nullptr;
};

}  // namespace pokepod
