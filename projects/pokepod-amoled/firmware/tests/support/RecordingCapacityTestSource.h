#pragma once

#include <cstdint>

#include "RecordingCapacitySource.h"

namespace pokepod {

class TestRecordingCapacitySource final : public RecordingCapacitySource {
 public:
  RecordingSpaceSnapshot snapshot{
      kRecordingRequiredFreeBytes + 8ULL * 1024ULL * 1024ULL, 0, true};
  uint32_t queryCalls = 0;
  uint32_t monotonicCalls = 0;
  uint64_t currentUs = 0;
  uint64_t normalStepUs = 100;
  uint32_t delayedCall = 0;
  uint64_t delayedStepUs = 0;

  RecordingSpaceSnapshot query() override {
    ++queryCalls;
    return snapshot;
  }

  uint64_t monotonicMicros() override {
    ++monotonicCalls;
    currentUs += monotonicCalls == delayedCall ? delayedStepUs : normalStepUs;
    return currentUs;
  }

  void resetTiming() {
    monotonicCalls = 0;
    currentUs = 0;
    normalStepUs = 100;
    delayedCall = 0;
    delayedStepUs = 0;
  }
};

}  // namespace pokepod
