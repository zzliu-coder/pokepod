#include <cassert>
#include <cstdint>
#include <memory>

#include "../PokePodAmoled/StorageCoordinator.cpp"
#include "../PokePodAmoled/CapsuleTransaction.cpp"
#include "../PokePodAmoled/WavRecorder.cpp"
#include "support/RecordingCapacityTestSource.h"

using namespace pokepod;

namespace {

constexpr char kId[] = "12345678-1234-4abc-8def-1234567890ab";
constexpr char kSecondId[] = "abcdef12-3456-4abc-8def-1234567890ab";
constexpr char kThirdId[] = "fedcba98-7654-4abc-8def-1234567890ab";
constexpr char kFourthId[] = "11223344-5566-4abc-8def-1234567890ab";
constexpr char kCreatedAt[] = "2026-08-11T05:40:21Z";

class QuietPrint final : public Print {};
class DenyGate final : public CapsuleTransactionGate {
 public:
  bool permits(uint32_t) override { return false; }
};

void finishRecovery(WavRecorder &recorder, Print &log) {
  uint32_t polls = 0;
  while (recorder.recoveryPending() && polls++ < 10000U) {
    (void)recorder.pollFinalize(log, polls, nullptr);
  }
  assert(polls < 10000U);
  assert(recorder.takeRecoveryReady());
}

void drain(WavRecorder &recorder, Print &log) {
  uint32_t polls = 0;
  while (recorder.operationActive() && polls++ < 20000U) {
    (void)recorder.pollFinalize(log, polls, nullptr);
  }
  assert(polls < 20000U);
}

RecorderOutcome admissionOutcome(const RecordingSpaceSnapshot &snapshot) {
  QuietPrint log;
  auto state = std::make_shared<fakefs::State>();
  fs::FS storage(state);
  TestRecordingCapacitySource source;
  source.snapshot = snapshot;
  WavRecorder recorder;
  assert(recorder.begin(storage, source, log));
  finishRecovery(recorder, log);
  assert(!recorder.start(log, kId, kCreatedAt));
  drain(recorder, log);
  assert(source.queryCalls == 1U);
  assert(state->openHandles == 0U);
  assert(StorageCoordinator::instance().mutationOwner() == StorageOwner::none);
  return recorder.terminalResult();
}

void runCapacityBoundaries() {
  RecorderOutcome outcome = admissionOutcome({0, 0, false});
  assert(outcome.terminal == RecorderTerminal::admissionFailure);
  assert(outcome.failureStage == RecorderFailureStage::capacityUnknown);

  outcome = admissionOutcome({100, 101, true});
  assert(outcome.failureStage == RecorderFailureStage::capacityInvalid);

  outcome = admissionOutcome(
      {kRecordingRequiredFreeBytes - 1U, 0, true});
  assert(outcome.failureStage == RecorderFailureStage::insufficientSpace);

  QuietPrint log;
  auto state = std::make_shared<fakefs::State>();
  fs::FS storage(state);
  TestRecordingCapacitySource source;
  source.snapshot = {kRecordingRequiredFreeBytes, 0, true};
  WavRecorder recorder;
  assert(recorder.begin(storage, source, log));
  finishRecovery(recorder, log);
  assert(recorder.start(log, kId, kCreatedAt));
  assert(source.queryCalls == 1U);
  assert(recorder.abortCapture(log));
  drain(recorder, log);
}

void runStorageOwnershipAndChangingCapacity() {
  QuietPrint log;
  auto state = std::make_shared<fakefs::State>();
  fs::FS storage(state);
  TestRecordingCapacitySource source;
  WavRecorder recorder;
  assert(recorder.begin(storage, source, log));
  finishRecovery(recorder, log);

  StorageReservation blocker = StorageCoordinator::instance().reserve(
      StorageOwner::capsuleScan, StorageAccess::mutation, 0);
  assert(blocker);
  assert(!recorder.start(log, kId, kCreatedAt));
  drain(recorder, log);
  assert(source.queryCalls == 0U);
  assert(recorder.terminalResult().failureStage ==
         RecorderFailureStage::storageBusy);
  RecorderOutcome acknowledged;
  assert(recorder.takeTerminalResult(acknowledged));
  blocker.release();

  // The platform value can change immediately after query(), but recorder
  // admission uses exactly the one snapshot obtained under its reservation.
  source.snapshot = {kRecordingRequiredFreeBytes + 4096U, 0, true};
  assert(recorder.start(log, kSecondId, kCreatedAt,
                        RecorderOperationOwner::linkUsb));
  source.snapshot = {0, 0, false};
  assert(source.queryCalls == 1U);
  assert(recorder.abortCapture(log));
  drain(recorder, log);
}

void runPerformanceAdmission() {
  {
    QuietPrint log;
    auto state = std::make_shared<fakefs::State>();
    fs::FS storage(state);
    TestRecordingCapacitySource source;
    // Aggregate 128 KiB probe throughput falls below 2x audio bitrate.
    source.normalStepUs = 50000U;
    WavRecorder recorder;
    assert(recorder.begin(storage, source, log));
    finishRecovery(recorder, log);
    assert(!recorder.start(log, kId, kCreatedAt));
    drain(recorder, log);
    assert(recorder.terminalResult().failureStage ==
           RecorderFailureStage::storageTooSlow);
    assert(state->openHandles == 0U);
  }
  {
    QuietPrint log;
    auto state = std::make_shared<fakefs::State>();
    fs::FS storage(state);
    TestRecordingCapacitySource source;
    // Qualification time is read before the probe; call 4 is the first
    // write+flush completion timestamp.
    source.delayedCall = 4U;
    source.delayedStepUs = kRecordingProbeMaximumTailUs + 1U;
    WavRecorder recorder;
    assert(recorder.begin(storage, source, log));
    finishRecovery(recorder, log);
    assert(!recorder.start(log, kId, kCreatedAt));
    drain(recorder, log);
    assert(recorder.terminalResult().failureStage ==
           RecorderFailureStage::storageTooSlow);
  }
  {
    QuietPrint log;
    auto state = std::make_shared<fakefs::State>();
    fs::FS storage(state);
    TestRecordingCapacitySource source;
    WavRecorder recorder;
    assert(recorder.begin(storage, source, log));
    finishRecovery(recorder, log);
    state->fail(fakefs::Operation::write, 1,
                fakefs::FaultAction::returnFailure);
    assert(!recorder.start(log, kId, kCreatedAt));
    state->clearFault();
    drain(recorder, log);
    assert(recorder.terminalResult().failureStage ==
           RecorderFailureStage::storageProbeWrite);
  }
}

void runQualificationCacheAndMountGeneration() {
  QuietPrint log;
  auto state = std::make_shared<fakefs::State>();
  fs::FS storage(state);
  TestRecordingCapacitySource source;
  WavRecorder recorder;
  assert(recorder.begin(storage, source, log));
  finishRecovery(recorder, log);

  assert(recorder.start(log, kThirdId, kCreatedAt));
  const uint32_t firstProbeCalls = source.monotonicCalls;
  assert(firstProbeCalls > 60U);
  assert(source.queryCalls == 1U);
  assert(recorder.qualificationSnapshot().qualified);
  assert(recorder.abortCapture(log));
  drain(recorder, log);
  RecorderOutcome first;
  assert(recorder.takeTerminalResult(first));

  assert(recorder.start(log, kSecondId, kCreatedAt));
  assert(source.queryCalls == 2U);
  assert(source.monotonicCalls == firstProbeCalls + 1U);
  assert(recorder.abortCapture(log));
  drain(recorder, log);
  RecorderOutcome second;
  assert(recorder.takeTerminalResult(second));

  ++source.generation;
  assert(recorder.start(log, kId, kCreatedAt));
  assert(source.queryCalls == 3U);
  assert(source.monotonicCalls > firstProbeCalls * 2U);
  assert(recorder.abortCapture(log));
  drain(recorder, log);
  RecorderOutcome third;
  assert(recorder.takeTerminalResult(third));

  source.generation = 0;
  assert(!recorder.start(log, kFourthId, kCreatedAt));
  drain(recorder, log);
  assert(recorder.terminalResult().failureStage ==
         RecorderFailureStage::capacityUnknown);
}

void runDeniedStartAckCleansResources() {
  QuietPrint log;
  auto state = std::make_shared<fakefs::State>();
  fs::FS storage(state);
  TestRecordingCapacitySource source;
  WavRecorder recorder;
  assert(recorder.begin(storage, source, log));
  finishRecovery(recorder, log);
  assert(recorder.requestStart(log, kId, kCreatedAt,
                               RecorderOperationOwner::linkWifi));
  DenyGate deny;
  assert(recorder.pollStart(log, 299000U, &deny) ==
         RecorderStartPollResult::cancelled);
  drain(recorder, log);
  assert(recorder.terminalResult().terminal == RecorderTerminal::cancelled);
  assert(state->openHandles == 0U);
  assert(StorageCoordinator::instance().mutationOwner() == StorageOwner::none);
  RecorderOutcome acknowledged;
  assert(recorder.takeTerminalResult(acknowledged));
  source.resetTiming();
  assert(recorder.start(log, kSecondId, kCreatedAt));
  assert(recorder.abortCapture(log));
  drain(recorder, log);
}

}  // namespace

int main() {
  runCapacityBoundaries();
  runStorageOwnershipAndChangingCapacity();
  runPerformanceAdmission();
  runQualificationCacheAndMountGeneration();
  runDeniedStartAckCleansResources();
  return 0;
}
