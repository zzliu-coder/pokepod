#pragma once

#include <stdint.h>

namespace pokepod {

// A performance qualification belongs to one physical SD mount only. Capacity
// remains a live admission fact and is deliberately absent from this cache.
constexpr uint64_t kRecordingQualificationMaximumAgeUs =
    6ULL * 60ULL * 60ULL * 1000000ULL;

enum class RecordingQualificationInvalidReason : uint8_t {
  none = 0,
  mountUnavailable,
  mountChanged,
  expired,
  probeFailed,
  capacityUnknown,
  capacityInvalid,
  lowSpace,
  shortWrite,
  ioError,
  queueHighWater,
};

enum class RecordingQualificationDecision : uint8_t {
  probe = 0,
  reuse,
  rejectUnavailable,
};

struct RecordingQualificationSnapshot {
  uint32_t mountGeneration = 0;
  uint64_t qualifiedAtUs = 0;
  uint64_t probeTotalUs = 0;
  uint64_t probeMaximumTailUs = 0;
  RecordingQualificationInvalidReason invalidReason =
      RecordingQualificationInvalidReason::mountUnavailable;
  bool qualified = false;
  bool failed = false;
};

class RecordingStorageQualification {
 public:
  RecordingQualificationDecision decision(uint32_t mountGeneration,
                                           uint64_t nowUs) {
    if (mountGeneration == 0) {
      invalidate(RecordingQualificationInvalidReason::mountUnavailable);
      return RecordingQualificationDecision::rejectUnavailable;
    }
    if (value_.mountGeneration != mountGeneration) {
      value_ = {};
      value_.mountGeneration = mountGeneration;
      value_.invalidReason = RecordingQualificationInvalidReason::mountChanged;
      return RecordingQualificationDecision::probe;
    }
    // A failed result is never reused as success. The next admission performs
    // a real probe; only that bounded retry can clear the failed fact.
    if (value_.failed) return RecordingQualificationDecision::probe;
    if (!value_.qualified) return RecordingQualificationDecision::probe;
    if (nowUs - value_.qualifiedAtUs > kRecordingQualificationMaximumAgeUs) {
      invalidate(RecordingQualificationInvalidReason::expired);
      return RecordingQualificationDecision::probe;
    }
    return RecordingQualificationDecision::reuse;
  }

  void recordSuccess(uint32_t mountGeneration, uint64_t nowUs,
                     uint64_t totalUs, uint64_t maximumTailUs) {
    value_.mountGeneration = mountGeneration;
    value_.qualifiedAtUs = nowUs;
    value_.probeTotalUs = totalUs;
    value_.probeMaximumTailUs = maximumTailUs;
    value_.invalidReason = RecordingQualificationInvalidReason::none;
    value_.qualified = mountGeneration != 0;
    value_.failed = false;
  }

  void recordProbeFailure(uint32_t mountGeneration) {
    value_.mountGeneration = mountGeneration;
    value_.qualified = false;
    value_.failed = mountGeneration != 0;
    value_.invalidReason = RecordingQualificationInvalidReason::probeFailed;
  }

  void invalidate(RecordingQualificationInvalidReason reason) {
    value_.qualified = false;
    value_.failed = false;
    value_.invalidReason = reason;
  }

  const RecordingQualificationSnapshot &snapshot() const { return value_; }

 private:
  RecordingQualificationSnapshot value_{};
};

}  // namespace pokepod
