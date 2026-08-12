#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

#include "FS.h"

#include "../PokePodAmoled/StorageCoordinator.cpp"
#include "../PokePodAmoled/CapsuleTransaction.cpp"
#include "../PokePodAmoled/WavRecorder.cpp"
#include "support/RecordingCapacityTestSource.h"

using namespace pokepod;

namespace {

constexpr const char *kRaw =
    "/PokeCapsule/Inbox/12345678-1234-4abc-8def-1234567890ab/raw.txt";
constexpr const char *kProcessing =
    "/PokeCapsule/Inbox/12345678-1234-4abc-8def-1234567890ab/processing.json";
constexpr const char *kOldRaw = "old raw";
constexpr const char *kOldProcessing = "{\"status\":\"old\"}";
constexpr const char *kNewRaw = "new raw text";
constexpr const char *kNewProcessing = "{\"status\":\"completed\"}";
constexpr const char *kFirstId = "12345678-1234-4abc-8def-1234567890ab";
constexpr const char *kSecondId = "abcdef12-3456-4abc-8def-1234567890ab";
constexpr const char *kCreatedAt = "2026-08-11T05:40:21Z";

struct QuietPrint final : public Print {};

uint32_t nextRandom(uint32_t &state) {
  state ^= state << 13;
  state ^= state >> 17;
  state ^= state << 5;
  return state;
}

void seedTransactionPair(const std::shared_ptr<fakefs::State> &state) {
  state->seed(kRaw, kOldRaw);
  state->seed(kProcessing, kOldProcessing);
}

void assertPairCoherent(const std::shared_ptr<fakefs::State> &state) {
  const std::string raw = state->text(kRaw);
  const std::string processing = state->text(kProcessing);
  const bool oldPair = raw == kOldRaw && processing == kOldProcessing;
  const bool newPair = raw == kNewRaw && processing == kNewProcessing;
  assert(oldPair || newPair);
  assert(state->files.count(kRaw) == 1);
  assert(state->files.count(kProcessing) == 1);
}

bool recoverTwice(fs::FS &storage, Print &log,
                  StorageCoordinator &coordinator) {
  CapsuleTransaction recovery;
  if (!recovery.begin(storage, log, coordinator)) return false;
  return recovery.recoverAll(StorageOwner::recovery) &&
      recovery.recoverAll(StorageOwner::recovery);
}

void runDeterministicTransactionFaults() {
  QuietPrint log;
  constexpr std::array<fakefs::Operation, 6> operations = {
      fakefs::Operation::open,
      fakefs::Operation::write,
      fakefs::Operation::flush,
      fakefs::Operation::rename,
      fakefs::Operation::remove,
      fakefs::Operation::close,
  };
  constexpr std::array<fakefs::FaultAction, 4> actions = {
      fakefs::FaultAction::returnFailure,
      fakefs::FaultAction::shortWrite,
      fakefs::FaultAction::crashBefore,
      fakefs::FaultAction::crashAfter,
  };

  uint32_t random = 0x6a09e667U;
  for (size_t sequence = 0; sequence < 10000; ++sequence) {
    auto state = std::make_shared<fakefs::State>();
    seedTransactionPair(state);
    fs::FS storage(state);
    StorageCoordinator coordinator;
    CapsuleTransaction transaction;
    assert(transaction.begin(storage, log, coordinator));

    const uint32_t bits = nextRandom(random);
    const fakefs::Operation operation = operations[bits % operations.size()];
    const fakefs::FaultAction action =
        actions[(bits >> 5U) % actions.size()];
    const uint32_t occurrence = 1U + ((bits >> 9U) % 12U);
    state->fail(operation, occurrence, action);
    try {
      transaction.commitTextPair(
          "fault-model", kRaw, kNewRaw, kProcessing, kNewProcessing,
          StorageOwner::capsuleTransaction);
    } catch (const fakefs::SimulatedCrash &) {
      // A reboot destroys the process objects. The shared fake storage is the
      // only retained state and is recovered through the public service.
    }
    state->clearFault();
    assert(recoverTwice(storage, log, coordinator));
    assertPairCoherent(state);
    assert(coordinator.mutationOwner() == StorageOwner::none);
  }
}

uint32_t countTransactionOperation(fakefs::Operation operation) {
  QuietPrint log;
  auto state = std::make_shared<fakefs::State>();
  seedTransactionPair(state);
  fs::FS storage(state);
  StorageCoordinator coordinator;
  CapsuleTransaction transaction;
  assert(transaction.begin(storage, log, coordinator));
  state->fail(operation, UINT32_MAX, fakefs::FaultAction::returnFailure);
  assert(transaction.commitTextPair(
      "cutpoints", kRaw, kNewRaw, kProcessing, kNewProcessing,
      StorageOwner::capsuleTransaction));
  return state->fault.seen;
}

void runEveryTransactionCutpoint() {
  QuietPrint log;
  constexpr std::array<fakefs::Operation, 6> operations = {
      fakefs::Operation::open,
      fakefs::Operation::write,
      fakefs::Operation::flush,
      fakefs::Operation::rename,
      fakefs::Operation::remove,
      fakefs::Operation::close,
  };
  for (const fakefs::Operation operation : operations) {
    const uint32_t count = countTransactionOperation(operation);
    assert(count > 0);
    for (uint32_t cutpoint = 1; cutpoint <= count; ++cutpoint) {
      for (const fakefs::FaultAction action : {
               fakefs::FaultAction::crashBefore,
               fakefs::FaultAction::crashAfter}) {
        auto state = std::make_shared<fakefs::State>();
        seedTransactionPair(state);
        fs::FS storage(state);
        StorageCoordinator coordinator;
        CapsuleTransaction transaction;
        assert(transaction.begin(storage, log, coordinator));
        state->fail(operation, cutpoint, action);
        try {
          transaction.commitTextPair(
              "cutpoints", kRaw, kNewRaw, kProcessing, kNewProcessing,
              StorageOwner::capsuleTransaction);
        } catch (const fakefs::SimulatedCrash &) {
        }
        state->clearFault();
        assert(recoverTwice(storage, log, coordinator));
        assertPairCoherent(state);
      }
    }
  }
}

void runTransactionEdgeCases() {
  QuietPrint log;
  auto state = std::make_shared<fakefs::State>();
  seedTransactionPair(state);
  fs::FS storage(state);
  StorageCoordinator coordinator;
  CapsuleTransaction transaction;
  assert(transaction.begin(storage, log, coordinator));

  assert(!transaction.writeTextAtomic(
      "/PokeCapsule/../escape", "bad", StorageOwner::capsuleTransaction,
      "bad-path"));
  assertPairCoherent(state);

  state->fail(fakefs::Operation::write, 2,
              fakefs::FaultAction::returnFailure);
  assert(!transaction.commitTextPair(
      "raw-first", kRaw, kNewRaw, kProcessing, kNewProcessing,
      StorageOwner::capsuleTransaction));
  state->clearFault();
  assert(recoverTwice(storage, log, coordinator));
  assertPairCoherent(state);

  state->fail(fakefs::Operation::write, 2,
              fakefs::FaultAction::returnFailure);
  assert(!transaction.commitTextPair(
      "processing-first", kProcessing, kNewProcessing, kRaw, kNewRaw,
      StorageOwner::capsuleTransaction));
  state->clearFault();
  assert(recoverTwice(storage, log, coordinator));
  assertPairCoherent(state);
  assert(transaction.commitTextPair(
      "retry-after-partial", kRaw, kNewRaw, kProcessing, kNewProcessing,
      StorageOwner::capsuleTransaction));
  assertPairCoherent(state);

  state->seed(kRaw, kOldRaw);
  state->seed(kProcessing, kOldProcessing);

  state->fail(fakefs::Operation::rename, 1,
              fakefs::FaultAction::crashAfter);
  try {
    transaction.commitTextPair(
        "corrupt-journal", kRaw, kNewRaw, kProcessing, kNewProcessing,
        StorageOwner::capsuleTransaction);
    assert(false && "journal rename crash must interrupt commit");
  } catch (const fakefs::SimulatedCrash &) {
  }
  state->clearFault();

  std::string journalPath;
  for (const auto &entry : state->files) {
    if (entry.first.size() >= 8 &&
        entry.first.compare(entry.first.size() - 8, 8, ".journal") == 0) {
      journalPath = entry.first;
      break;
    }
  }
  assert(!journalPath.empty());
  assert(!state->files[journalPath].empty());
  state->files[journalPath][0] ^= 0x80U;

  CapsuleTransaction corruptedRecovery;
  assert(corruptedRecovery.begin(storage, log, coordinator));
  assert(!corruptedRecovery.recoverAll(StorageOwner::recovery));
  assertPairCoherent(state);
}

TestRecordingCapacitySource capacitySource;

void prepareRecorder(fs::FS &storage, WavRecorder &recorder, Print &log) {
  assert(recorder.begin(storage, capacitySource, log));
  uint32_t polls = 0;
  while (recorder.recoveryPending() && polls++ < 10000U) {
    (void)recorder.pollFinalize(log, polls, nullptr);
  }
  assert(polls < 10000U);
  assert(recorder.takeRecoveryReady());
}

void drainRecorder(WavRecorder &recorder, Print &log) {
  uint32_t polls = 0;
  while (recorder.operationActive() && polls++ < 20000U) {
    (void)recorder.pollFinalize(log, polls, nullptr);
  }
  assert(polls < 20000U);
  assert(!recorder.operationActive());
}

void assertRecorderFault(fakefs::Operation operation,
                         fakefs::FaultAction action,
                         RecorderFailureStage expectedStage) {
  QuietPrint log;
  auto state = std::make_shared<fakefs::State>();
  fs::FS storage(state);
  WavRecorder recorder;
  prepareRecorder(storage, recorder, log);
  assert(recorder.start(log, kFirstId, kCreatedAt));

  std::array<int16_t, 320> samples{};
  for (size_t index = 0; index < samples.size(); ++index) {
    samples[index] = static_cast<int16_t>((index * 97U) % 10000U);
  }

  if (operation == fakefs::Operation::write) {
    state->fail(operation, 1, action);
    assert(!recorder.appendMono16(samples.data(), samples.size(), log));
  } else {
    assert(recorder.appendMono16(samples.data(), samples.size(), log));
    if (operation == fakefs::Operation::rename ||
        operation == fakefs::Operation::remove) {
      state->failAlways(operation, action);
    } else {
      state->fail(operation, 1, action);
    }
    assert(recorder.stop(log));
  }
  drainRecorder(recorder, log);
  state->clearFault();

  const RecorderOutcome outcome = recorder.terminalResult();
  assert(outcome.pending());
  assert(!outcome.success());
  assert(outcome.failureStage == expectedStage);
  const std::string inbox = std::string(kCapsuleInbox) + "/" + kFirstId;
  assert(state->directories.count(inbox) == 0);
  assert(state->text(inbox + "/processing.json").empty());
}

void assertRecorderShortWriteAt(uint32_t occurrence) {
  QuietPrint log;
  auto state = std::make_shared<fakefs::State>();
  fs::FS storage(state);
  WavRecorder recorder;
  prepareRecorder(storage, recorder, log);
  assert(recorder.start(log, kFirstId, kCreatedAt));
  std::array<int16_t, 320> samples{};
  state->fail(fakefs::Operation::write, occurrence,
              fakefs::FaultAction::shortWrite);
  for (uint32_t block = 1; block <= occurrence; ++block) {
    const bool appended =
        recorder.appendMono16(samples.data(), samples.size(), log);
    assert(appended == (block < occurrence));
  }
  state->clearFault();
  drainRecorder(recorder, log);
  const RecorderOutcome outcome = recorder.terminalResult();
  assert(outcome.terminal == RecorderTerminal::storageFailure);
  assert(outcome.failureStage == RecorderFailureStage::shortWrite);
  const uint32_t blockBytes = samples.size() * sizeof(int16_t);
  assert(outcome.dataBytes >= (occurrence - 1U) * blockBytes);
  assert(outcome.dataBytes < occurrence * blockBytes);
}

void runRecorderOpenFailure() {
  QuietPrint log;
  auto state = std::make_shared<fakefs::State>();
  fs::FS storage(state);
  WavRecorder recorder;
  prepareRecorder(storage, recorder, log);
  state->failAlways(fakefs::Operation::open,
                    fakefs::FaultAction::returnFailure);
  assert(!recorder.start(log, kFirstId, kCreatedAt));
  state->clearFault();
  drainRecorder(recorder, log);
  const RecorderOutcome outcome = recorder.terminalResult();
  assert(outcome.terminal == RecorderTerminal::storageFailure);
  assert(outcome.failureStage == RecorderFailureStage::storageProbeOpen);
}

void runRecorderFaultsAndReset() {
  runRecorderOpenFailure();
  assertRecorderShortWriteAt(1);
  assertRecorderShortWriteAt(2);
  assertRecorderShortWriteAt(3);
  assertRecorderFault(fakefs::Operation::flush,
                      fakefs::FaultAction::returnFailure,
                      RecorderFailureStage::flushAudio);
  assertRecorderFault(fakefs::Operation::seek,
                      fakefs::FaultAction::returnFailure,
                      RecorderFailureStage::finalHeader);
  assertRecorderFault(fakefs::Operation::close,
                      fakefs::FaultAction::returnFailure,
                      RecorderFailureStage::commitAudio);
  assertRecorderFault(fakefs::Operation::rename,
                      fakefs::FaultAction::returnFailure,
                      RecorderFailureStage::commitAudio);
  assertRecorderFault(fakefs::Operation::remove,
                      fakefs::FaultAction::returnFailure,
                      RecorderFailureStage::commitAudio);

  QuietPrint log;
  auto state = std::make_shared<fakefs::State>();
  fs::FS storage(state);
  WavRecorder recorder;
  prepareRecorder(storage, recorder, log);
  assert(recorder.start(log, kFirstId, kCreatedAt));
  std::array<int16_t, 320> first{};
  state->fail(fakefs::Operation::write, 1,
              fakefs::FaultAction::shortWrite);
  assert(!recorder.appendMono16(first.data(), first.size(), log));
  state->clearFault();
  drainRecorder(recorder, log);

  RecorderOutcome acknowledged;
  assert(recorder.takeTerminalResult(acknowledged));
  assert(recorder.start(log, kSecondId, kCreatedAt));
  assert(recorder.durationMs() == 0);
  assert(recorder.finalPath().endsWith("audio.wav"));
  std::array<int16_t, 640> second{};
  for (size_t index = 0; index < second.size(); ++index) {
    second[index] = static_cast<int16_t>(index + 1);
  }
  assert(recorder.appendMono16(second.data(), second.size(), log));
  assert(recorder.stop(log));
  drainRecorder(recorder, log);
  const RecorderOutcome outcome = recorder.terminalResult();
  assert(outcome.success());
  assert(outcome.dataBytes == second.size() * sizeof(int16_t));
  const std::string secondInbox =
      std::string(kCapsuleInbox) + "/" + kSecondId;
  assert(state->directories.count(secondInbox) == 1);
  assert(state->text(secondInbox + "/processing.json").find("queued") !=
         std::string::npos);
}

void runCorruptCheckpointRecovery() {
  QuietPrint log;
  auto state = std::make_shared<fakefs::State>();
  fs::FS storage(state);
  WavRecorder recorder;
  prepareRecorder(storage, recorder, log);

  StoredRecorderCheckpoint checkpoint{};
  assert(initializeRecorderCheckpoint(checkpoint, kFirstId, kCreatedAt));
  updateRecorderCheckpoint(checkpoint, 64000U, 0x12345678U);
  checkpoint.crc32 ^= 1U;

  const std::string staging =
      std::string(kCapsuleStaging) + "/" + kFirstId;
  state->directories.insert(staging);
  state->seedBytes(staging + "/recording.chk",
                   reinterpret_cast<const uint8_t *>(&checkpoint),
                   sizeof(checkpoint));
  std::array<uint8_t, kWavHeaderBytes + 64000U> partial{};
  state->seedBytes(staging + "/audio.wav.part", partial.data(), partial.size());

  WavRecorder rebooted;
  assert(rebooted.begin(storage, capacitySource, log));
  uint32_t polls = 0;
  while (rebooted.recoveryPending() && polls++ < 10000U) {
    (void)rebooted.pollFinalize(log, polls, nullptr);
  }
  assert(polls < 10000U);
  assert(!rebooted.recoveryFailed());
  assert(rebooted.takeRecoveryReady());
  const std::string inbox = std::string(kCapsuleInbox) + "/" + kFirstId;
  assert(state->directories.count(inbox) == 0);
  assert(state->directories.count(staging + ".blocked") == 1);
}

void runRecorderDeferredCleanupAcrossContexts() {
  QuietPrint log;
  auto state = std::make_shared<fakefs::State>();
  fs::FS storage(state);
  WavRecorder recorder;
  prepareRecorder(storage, recorder, log);
  assert(recorder.start(log, kFirstId, kCreatedAt));
  std::array<int16_t, 320> samples{};
  assert(recorder.appendMono16(samples.data(), samples.size(), log));

  state->fail(fakefs::Operation::close, UINT32_MAX,
              fakefs::FaultAction::returnFailure);
  std::atomic<bool> aborted{true};
  std::thread foreignContext([&]() {
    aborted.store(recorder.abortCapture(log));
  });
  foreignContext.join();
  assert(aborted.load());
  assert(recorder.cleanupPending());
  assert(!recorder.terminalResult().pending());
  assert(StorageCoordinator::instance().mutationOwner() ==
         StorageOwner::recorder);
  assert(state->fault.seen == 0);

  // The original reservation context can later perform the physical close,
  // publish the terminal result and release storage for the next session.
  while (recorder.cleanupPending()) (void)recorder.pollCleanup(log);
  assert(state->fault.seen > 0);
  state->clearFault();
  assert(!recorder.cleanupPending());
  assert(recorder.terminalResult().failureStage ==
         RecorderFailureStage::captureIncomplete);
  assert(StorageCoordinator::instance().mutationOwner() == StorageOwner::none);
  RecorderOutcome acknowledged;
  assert(recorder.takeTerminalResult(acknowledged));
  assert(recorder.start(log, kSecondId, kCreatedAt));
  assert(recorder.abortCapture(log));
  drainRecorder(recorder, log);
  assert(!recorder.cleanupPending());
}

}  // namespace

int main() {
  runDeterministicTransactionFaults();
  runEveryTransactionCutpoint();
  runTransactionEdgeCases();
  runRecorderFaultsAndReset();
  runCorruptCheckpointRecovery();
  runRecorderDeferredCleanupAcrossContexts();

  const RecordingSpaceSnapshot oneByteShort{
      kRecordingRequiredFreeBytes - 1U, 0, true};
  const RecordingSpaceSnapshot exact{kRecordingRequiredFreeBytes, 0, true};
  const RecordingSpaceSnapshot reservePlus{
      kRecordingRequiredFreeBytes + kRecordingSafetyReserveBytes, 0, true};
  assert(!evaluateRecordingAdmission(oneByteShort).allowed());
  assert(evaluateRecordingAdmission(exact).allowed());
  assert(evaluateRecordingAdmission(reservePlus).allowed());

  std::cout << "transaction_sequences=10000 all_cutpoints=covered "
               "recorder_fault_scenarios=10 checkpoint_corrupt=1\n";
  return 0;
}
