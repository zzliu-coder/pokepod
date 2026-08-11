#include <cassert>
#include <cstring>

#include "RecorderCheckpoint.h"
#include "RecorderOutcome.h"
#include "RecordingAdmissionPolicy.h"

using namespace pokepod;

int main() {
  static_assert(kMaximumRecordingAudioBytes == 1872000ULL);
  static_assert(kMaximumRecordingWavBytes == 1872044ULL);
  static_assert(kRecordingSafetyReserveBytes == 512ULL * 1024ULL);
  static_assert(kRecorderCheckpointIntervalBytes == 64000U);

  RecorderOutcomeState terminal;
  assert(!terminal.peek().pending());
  terminal.fail(RecorderTerminal::storageFailure,
                RecorderFailureStage::shortWrite, 32000);
  assert(terminal.peek().pending());
  assert(!terminal.peek().success());
  assert(terminal.peek().dataBytes == 32000);
  assert(terminal.peek().failureStage == RecorderFailureStage::shortWrite);
  terminal.reset();
  assert(!terminal.peek().pending());
  assert(terminal.peek().dataBytes == 0);
  terminal.complete(RecorderStopReason::maxDuration,
                    static_cast<uint32_t>(kMaximumRecordingAudioBytes));
  RecorderOutcome completed;
  assert(terminal.take(completed));
  assert(completed.success());
  assert(completed.stopReason == RecorderStopReason::maxDuration);
  assert(completed.failureStage == RecorderFailureStage::none);
  assert(!terminal.take(completed));
  terminal.fail(RecorderTerminal::completed,
                RecorderFailureStage::shortWrite, 1);
  assert(!terminal.peek().pending());
  terminal.fail(RecorderTerminal::captureFailure,
                RecorderFailureStage::captureIncomplete, 6400);
  assert(terminal.peek().terminal == RecorderTerminal::captureFailure);
  assert(terminal.peek().failureStage ==
         RecorderFailureStage::captureIncomplete);
  assert(std::strcmp(recorderFailureStageName(
                         RecorderFailureStage::captureIncomplete),
                     "capture_incomplete") == 0);
  terminal.reset();
  assert(std::strcmp(recorderFailureStageName(
                         RecorderFailureStage::insufficientSpace),
                     "insufficient_space") == 0);

  RecordingSpaceSnapshot unknown{};
  assert(evaluateRecordingAdmission(unknown).reason ==
         RecordingAdmissionReason::capacityUnknown);
  const RecordingSpaceSnapshot invalid{100, 101, true};
  assert(evaluateRecordingAdmission(invalid).reason ==
         RecordingAdmissionReason::invalidCapacity);
  const RecordingSpaceSnapshot oneByteShort{
      kRecordingRequiredFreeBytes - 1, 0, true};
  assert(evaluateRecordingAdmission(oneByteShort).reason ==
         RecordingAdmissionReason::insufficientSpace);
  const RecordingSpaceSnapshot exact{kRecordingRequiredFreeBytes, 0, true};
  assert(evaluateRecordingAdmission(exact).allowed());
  const RecordingSpaceSnapshot withUsed{
      kRecordingRequiredFreeBytes + 4096, 4096, true};
  const RecordingAdmission admitted = evaluateRecordingAdmission(withUsed);
  assert(admitted.allowed());
  assert(admitted.availableBytes == kRecordingRequiredFreeBytes);

  constexpr const char *kId = "12345678-1234-4abc-8def-1234567890ab";
  constexpr const char *kCreatedAt = "2026-08-11T13:40:21+08:00";
  constexpr const char *kUtcCreatedAt = "2026-08-11T05:40:21Z";
  assert(recorderCheckpointCreatedAtShape(kCreatedAt));
  assert(recorderCheckpointCreatedAtShape(kUtcCreatedAt));
  assert(recorderCheckpointCreatedAtShape("2024-02-29T23:59:59Z"));
  assert(!recorderCheckpointCreatedAtShape("2026-02-29T13:40:21Z"));
  assert(!recorderCheckpointCreatedAtShape("2026-13-11T13:40:21Z"));
  assert(!recorderCheckpointCreatedAtShape("2026-08-32T13:40:21Z"));
  assert(!recorderCheckpointCreatedAtShape("2026-08-11T24:00:00Z"));
  assert(!recorderCheckpointCreatedAtShape("2026-08-11T13:40:60Z"));
  assert(!recorderCheckpointCreatedAtShape("2026-08-11 13:40:21Z"));
  assert(!recorderCheckpointCreatedAtShape("2026-08-11T13:40:21+14:01"));
  assert(!recorderCheckpointCreatedAtShape("2026-08-11T13:40:21+08"));
  StoredRecorderCheckpoint checkpoint{};
  assert(initializeRecorderCheckpoint(checkpoint, kId, kCreatedAt));
  assert(validateRecorderCheckpoint(checkpoint));
  assert(std::strcmp(checkpoint.capsuleId, kId) == 0);
  assert(std::strcmp(checkpoint.createdAt, kCreatedAt) == 0);
  assert(planRecorderRecovery(checkpoint, 0).disposition ==
         RecorderRecoveryDisposition::noAudio);

  assert(!recorderCheckpointDue(0, kRecorderCheckpointIntervalBytes - 2));
  assert(recorderCheckpointDue(0, kRecorderCheckpointIntervalBytes));
  assert(recorderCheckpointDue(64000, 128000));
  assert(!recorderCheckpointDue(128000, 64000));

  updateRecorderCheckpoint(checkpoint, 64001, 0x12345678U);
  assert(validateRecorderCheckpoint(checkpoint));
  assert(checkpoint.confirmedDataBytes == 64000);
  RecorderRecoveryPlan recovery = planRecorderRecovery(checkpoint, 70000);
  assert(recovery.disposition ==
         RecorderRecoveryDisposition::recoverConfirmedAudio);
  assert(recovery.recoveredDataBytes == 64000);
  assert(std::strcmp(checkpoint.createdAt, kCreatedAt) == 0);
  assert(planRecorderRecovery(checkpoint, 63998).disposition ==
         RecorderRecoveryDisposition::damaged);
  assert(planRecorderRecovery(checkpoint, 70001).disposition ==
         RecorderRecoveryDisposition::recoverConfirmedAudio);
  assert(planRecorderRecovery(checkpoint, 70000, checkpoint.audioCrc32 ^ 1U)
             .disposition == RecorderRecoveryDisposition::damaged);

  failRecorderCheckpoint(checkpoint, RecorderFailureStage::shortWrite);
  assert(validateRecorderCheckpoint(checkpoint));
  recovery = planRecorderRecovery(checkpoint, 70000);
  assert(recovery.disposition ==
         RecorderRecoveryDisposition::preserveFailure);
  assert(recovery.recoveredDataBytes == 64000);

  StoredRecorderCheckpoint corrupt = checkpoint;
  corrupt.audioCrc32 ^= 1U;
  assert(!validateRecorderCheckpoint(corrupt));
  corrupt = checkpoint;
  corrupt.confirmedDataBytes =
      static_cast<uint32_t>(kMaximumRecordingAudioBytes + 2ULL);
  finalizeRecorderCheckpoint(corrupt);
  assert(!validateRecorderCheckpoint(corrupt));
  corrupt = checkpoint;
  std::memset(corrupt.createdAt, 'x', sizeof(corrupt.createdAt));
  finalizeRecorderCheckpoint(corrupt);
  assert(!validateRecorderCheckpoint(corrupt));

  StoredRecorderCheckpoint invalidId{};
  assert(!initializeRecorderCheckpoint(invalidId, "not-a-uuid", kCreatedAt));
  assert(!initializeRecorderCheckpoint(invalidId, kId, ""));
  assert(!initializeRecorderCheckpoint(invalidId, kId,
                                       "2026-02-29T13:40:21Z"));
  return 0;
}
