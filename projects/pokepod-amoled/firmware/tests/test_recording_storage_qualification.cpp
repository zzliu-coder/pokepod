#include <cassert>

#include "RecordingStorageQualification.h"

using namespace pokepod;

int main() {
  RecordingStorageQualification policy;
  assert(policy.decision(0, 1) ==
         RecordingQualificationDecision::rejectUnavailable);
  assert(policy.decision(3, 100) == RecordingQualificationDecision::probe);
  policy.recordSuccess(3, 100, 40, 20);
  assert(policy.decision(3, 101) == RecordingQualificationDecision::reuse);
  assert(policy.decision(3, 100 + kRecordingQualificationMaximumAgeUs) ==
         RecordingQualificationDecision::reuse);
  assert(policy.decision(3, 101 + kRecordingQualificationMaximumAgeUs) ==
         RecordingQualificationDecision::probe);
  policy.recordSuccess(3, UINT64_MAX - 20U, 40, 20);
  assert(policy.decision(3, 10) == RecordingQualificationDecision::reuse);
  policy.invalidate(RecordingQualificationInvalidReason::shortWrite);
  assert(policy.decision(3, 11) == RecordingQualificationDecision::probe);
  policy.recordProbeFailure(3);
  assert(policy.decision(3, 12) == RecordingQualificationDecision::probe);
  assert(policy.snapshot().failed);
  assert(policy.decision(4, 13) == RecordingQualificationDecision::probe);
  return 0;
}
