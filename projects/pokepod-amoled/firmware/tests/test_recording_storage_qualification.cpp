#include <cassert>
#include <atomic>
#include <thread>

#include "RecordingStorageQualification.h"

using namespace pokepod;

int main() {
  RecordingStorageQualification policy;
  assert(policy.decision(0, 1) ==
         RecordingQualificationDecision::rejectUnavailable);
  assert(policy.decision(3, 100) == RecordingQualificationDecision::probe);
  uint32_t probeEpoch = policy.invalidationEpoch();
  assert(policy.recordSuccess(3, 100, 40, 20, probeEpoch));
  assert(policy.decision(3, 101) == RecordingQualificationDecision::reuse);
  assert(policy.decision(3, 100 + kRecordingQualificationMaximumAgeUs) ==
         RecordingQualificationDecision::reuse);
  assert(policy.decision(3, 101 + kRecordingQualificationMaximumAgeUs) ==
         RecordingQualificationDecision::probe);
  probeEpoch = policy.invalidationEpoch();
  assert(policy.recordSuccess(3, UINT64_MAX - 20U, 40, 20, probeEpoch));
  assert(policy.decision(3, 10) == RecordingQualificationDecision::reuse);
  policy.requestInvalidation(RecordingQualificationInvalidReason::shortWrite);
  assert(!policy.snapshot().qualified);
  assert(policy.decision(3, 11) == RecordingQualificationDecision::probe);
  probeEpoch = policy.invalidationEpoch();
  policy.recordProbeFailure(3, probeEpoch);
  assert(policy.decision(3, 12) == RecordingQualificationDecision::probe);
  assert(policy.snapshot().failed);
  assert(policy.decision(4, 13) == RecordingQualificationDecision::probe);

  RecordingStorageQualification concurrent;
  for (uint32_t iteration = 0; iteration < 50000U; ++iteration) {
    assert(concurrent.decision(9, iteration) ==
           RecordingQualificationDecision::probe);
    const uint32_t epoch = concurrent.invalidationEpoch();
    std::atomic<bool> start{false};
    std::thread pressure([&]() {
      while (!start.load(std::memory_order_acquire)) {}
      concurrent.requestInvalidation(
          RecordingQualificationInvalidReason::queueHighWater);
    });
    std::thread storageFailure([&]() {
      while (!start.load(std::memory_order_acquire)) {}
      concurrent.requestInvalidation(
          RecordingQualificationInvalidReason::shortWrite);
    });
    start.store(true, std::memory_order_release);
    // Simulate a probe completing while producer invalidations race it.
    (void)concurrent.recordSuccess(9, iteration + 1U, 40, 20, epoch);
    pressure.join();
    storageFailure.join();
    const RecordingQualificationSnapshot raced = concurrent.snapshot();
    assert(!raced.qualified);
    assert(concurrent.decision(9, iteration + 2U) ==
           RecordingQualificationDecision::probe);
  }
  return 0;
}
