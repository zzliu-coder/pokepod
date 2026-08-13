#pragma once

#include <stddef.h>
#include <stdint.h>

namespace pokepod {

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

}  // namespace pokepod
