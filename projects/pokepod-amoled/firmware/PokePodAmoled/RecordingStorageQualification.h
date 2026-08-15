#pragma once

#include <atomic>
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
  uint32_t invalidationEpoch = 0;
  RecordingQualificationInvalidReason invalidReason =
      RecordingQualificationInvalidReason::mountUnavailable;
  bool qualified = false;
  bool failed = false;
};

// Qualification truth has one owner: the recorder storage context. Capture/UI
// producers may only publish invalidation requests. The owner consumes those
// requests before admission, probes and writes. A request also advances an
// epoch immediately, so a probe that raced the request cannot later publish a
// successful stale qualification.
class RecordingStorageQualification {
 public:
  void requestInvalidation(RecordingQualificationInvalidReason reason) {
    const uint32_t bit = reasonBit(reason);
    if (bit == 0) return;
    latestInvalidReason_.store(static_cast<uint32_t>(reason),
                               std::memory_order_relaxed);
    invalidationEpoch_.fetch_add(1U, std::memory_order_acq_rel);
    pendingInvalidations_.fetch_or(bit, std::memory_order_release);
  }

  bool consumeInvalidationRequests() {
    uint32_t pending = pendingInvalidations_.load(std::memory_order_acquire);
    while (pending != 0 && !pendingInvalidations_.compare_exchange_weak(
        pending, 0, std::memory_order_acq_rel, std::memory_order_acquire)) {}
    if (pending == 0) return false;
    invalidateOwned(selectReason(pending), false);
    return true;
  }

  RecordingQualificationDecision decision(uint32_t mountGeneration,
                                           uint64_t nowUs) {
    (void)consumeInvalidationRequests();
    if (mountGeneration == 0) {
      invalidateOwned(RecordingQualificationInvalidReason::mountUnavailable,
                      true);
      return RecordingQualificationDecision::rejectUnavailable;
    }
    if (value_.mountGeneration != mountGeneration) {
      value_ = {};
      value_.mountGeneration = mountGeneration;
      value_.invalidationEpoch = invalidationEpoch();
      value_.invalidReason = RecordingQualificationInvalidReason::mountChanged;
      publishOwned();
      return RecordingQualificationDecision::probe;
    }
    // A failed result is never reused as success. The next admission performs
    // a real probe; only that bounded retry can clear the failed fact.
    if (value_.failed) return RecordingQualificationDecision::probe;
    if (!value_.qualified) return RecordingQualificationDecision::probe;
    if (nowUs - value_.qualifiedAtUs > kRecordingQualificationMaximumAgeUs) {
      invalidateOwned(RecordingQualificationInvalidReason::expired, true);
      return RecordingQualificationDecision::probe;
    }
    return RecordingQualificationDecision::reuse;
  }

  uint32_t invalidationEpoch() const {
    return invalidationEpoch_.load(std::memory_order_acquire);
  }

  bool recordSuccess(uint32_t mountGeneration, uint64_t nowUs,
                     uint64_t totalUs, uint64_t maximumTailUs,
                     uint32_t probeEpoch) {
    const bool invalidated = consumeInvalidationRequests();
    if (invalidated || probeEpoch != invalidationEpoch()) return false;
    value_.mountGeneration = mountGeneration;
    value_.qualifiedAtUs = nowUs;
    value_.probeTotalUs = totalUs;
    value_.probeMaximumTailUs = maximumTailUs;
    value_.invalidationEpoch = probeEpoch;
    value_.invalidReason = RecordingQualificationInvalidReason::none;
    value_.qualified = mountGeneration != 0;
    value_.failed = false;
    publishOwned();
    return value_.qualified;
  }

  void recordProbeFailure(uint32_t mountGeneration, uint32_t probeEpoch) {
    if (consumeInvalidationRequests() || probeEpoch != invalidationEpoch()) {
      return;
    }
    value_.mountGeneration = mountGeneration;
    value_.invalidationEpoch = probeEpoch;
    value_.qualified = false;
    value_.failed = mountGeneration != 0;
    value_.invalidReason = RecordingQualificationInvalidReason::probeFailed;
    publishOwned();
  }

  // Storage-owner-only invalidation. Cross-context callers use
  // requestInvalidation(), which never writes owner state.
  void invalidateOwned(RecordingQualificationInvalidReason reason) {
    invalidateOwned(reason, true);
  }

  RecordingQualificationSnapshot snapshot() const {
    RecordingQualificationSnapshot result;
    uint32_t before = 0;
    uint32_t after = 0;
    do {
      before = publishedSequence_.load(std::memory_order_acquire);
      if ((before & 1U) != 0) continue;
      result.mountGeneration =
          publishedMountGeneration_.load(std::memory_order_relaxed);
      result.qualifiedAtUs =
          publishedQualifiedAtUs_.load(std::memory_order_relaxed);
      result.probeTotalUs =
          publishedProbeTotalUs_.load(std::memory_order_relaxed);
      result.probeMaximumTailUs =
          publishedProbeMaximumTailUs_.load(std::memory_order_relaxed);
      result.invalidationEpoch =
          publishedInvalidationEpoch_.load(std::memory_order_relaxed);
      result.invalidReason = static_cast<RecordingQualificationInvalidReason>(
          publishedInvalidReason_.load(std::memory_order_relaxed));
      result.qualified =
          publishedQualified_.load(std::memory_order_relaxed) != 0;
      result.failed = publishedFailed_.load(std::memory_order_relaxed) != 0;
      after = publishedSequence_.load(std::memory_order_acquire);
    } while (before != after || (after & 1U) != 0);

    // Pending invalidation is already authoritative even if the storage owner
    // has not reached its next consumption point yet.
    const uint32_t pending =
        pendingInvalidations_.load(std::memory_order_acquire);
    const uint32_t epoch = invalidationEpoch();
    if (pending != 0 || result.invalidationEpoch != epoch) {
      result.invalidationEpoch = epoch;
      result.invalidReason = pending != 0
          ? selectReason(pending)
          : static_cast<RecordingQualificationInvalidReason>(
                latestInvalidReason_.load(std::memory_order_relaxed));
      result.qualified = false;
      result.failed = false;
    }
    return result;
  }

 private:
  static uint32_t reasonBit(RecordingQualificationInvalidReason reason) {
    const uint8_t index = static_cast<uint8_t>(reason);
    return index == 0 || index >= 32 ? 0U : (1UL << index);
  }

  static RecordingQualificationInvalidReason selectReason(uint32_t pending) {
    // Prefer physical I/O facts over pressure/capacity facts when requests
    // coincide; every selected reason still forces a new qualification.
    constexpr RecordingQualificationInvalidReason priority[] = {
        RecordingQualificationInvalidReason::shortWrite,
        RecordingQualificationInvalidReason::ioError,
        RecordingQualificationInvalidReason::queueHighWater,
        RecordingQualificationInvalidReason::lowSpace,
        RecordingQualificationInvalidReason::capacityInvalid,
        RecordingQualificationInvalidReason::capacityUnknown,
        RecordingQualificationInvalidReason::probeFailed,
        RecordingQualificationInvalidReason::expired,
        RecordingQualificationInvalidReason::mountChanged,
        RecordingQualificationInvalidReason::mountUnavailable,
    };
    for (const auto reason : priority) {
      if ((pending & reasonBit(reason)) != 0) return reason;
    }
    return RecordingQualificationInvalidReason::ioError;
  }

  void invalidateOwned(RecordingQualificationInvalidReason reason,
                       bool advanceEpoch) {
    latestInvalidReason_.store(static_cast<uint32_t>(reason),
                               std::memory_order_relaxed);
    if (advanceEpoch) {
      invalidationEpoch_.fetch_add(1U, std::memory_order_acq_rel);
    }
    value_.invalidationEpoch = invalidationEpoch();
    value_.qualified = false;
    value_.failed = false;
    value_.invalidReason = reason;
    publishOwned();
  }

  void publishOwned() {
    publishedSequence_.fetch_add(1U, std::memory_order_acq_rel);
    publishedMountGeneration_.store(value_.mountGeneration,
                                    std::memory_order_relaxed);
    publishedQualifiedAtUs_.store(value_.qualifiedAtUs,
                                  std::memory_order_relaxed);
    publishedProbeTotalUs_.store(value_.probeTotalUs,
                                 std::memory_order_relaxed);
    publishedProbeMaximumTailUs_.store(value_.probeMaximumTailUs,
                                       std::memory_order_relaxed);
    publishedInvalidationEpoch_.store(value_.invalidationEpoch,
                                      std::memory_order_relaxed);
    publishedInvalidReason_.store(
        static_cast<uint32_t>(value_.invalidReason),
        std::memory_order_relaxed);
    publishedQualified_.store(value_.qualified ? 1U : 0U,
                              std::memory_order_relaxed);
    publishedFailed_.store(value_.failed ? 1U : 0U,
                           std::memory_order_relaxed);
    publishedSequence_.fetch_add(1U, std::memory_order_release);
  }

  RecordingQualificationSnapshot value_{};
  std::atomic<uint32_t> pendingInvalidations_{0};
  std::atomic<uint32_t> invalidationEpoch_{0};
  std::atomic<uint32_t> latestInvalidReason_{
      static_cast<uint32_t>(
          RecordingQualificationInvalidReason::mountUnavailable)};
  mutable std::atomic<uint32_t> publishedSequence_{0};
  std::atomic<uint32_t> publishedMountGeneration_{0};
  std::atomic<uint64_t> publishedQualifiedAtUs_{0};
  std::atomic<uint64_t> publishedProbeTotalUs_{0};
  std::atomic<uint64_t> publishedProbeMaximumTailUs_{0};
  std::atomic<uint32_t> publishedInvalidationEpoch_{0};
  std::atomic<uint32_t> publishedInvalidReason_{
      static_cast<uint32_t>(
          RecordingQualificationInvalidReason::mountUnavailable)};
  std::atomic<uint32_t> publishedQualified_{0};
  std::atomic<uint32_t> publishedFailed_{0};
};

}  // namespace pokepod
