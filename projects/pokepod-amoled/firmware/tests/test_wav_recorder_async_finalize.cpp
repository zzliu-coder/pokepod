#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <atomic>

#include "../PokePodAmoled/StorageCoordinator.cpp"
#include "../PokePodAmoled/CapsuleTransaction.cpp"
#include "../PokePodAmoled/WavRecorder.cpp"

using namespace pokepod;

namespace {

constexpr char kFirstId[] = "12345678-1234-4abc-8def-1234567890ab";
constexpr char kSecondId[] = "abcdef12-3456-4abc-8def-1234567890ab";
constexpr char kThirdId[] = "fedcba98-7654-4abc-8def-1234567890ab";
constexpr char kCreatedAt[] = "2026-08-11T05:40:21Z";

class QuietPrint final : public Print {};

RecordingSpaceSnapshot admittedSpace() {
  return {kRecordingRequiredFreeBytes + 4096U, 0, true};
}

void finishBootRecovery(WavRecorder &recorder,
                        const std::shared_ptr<fakefs::State> &state,
                        Print &log) {
  uint32_t polls = 0;
  while (recorder.recoveryPending() && polls++ < 10000U) {
    const uint32_t before = state->operations;
    (void)recorder.pollFinalize(log, polls, nullptr);
    assert(state->operations - before <= 1U);
  }
  assert(polls < 10000U);
  assert(recorder.takeRecoveryReady());
}

void appendFrame(WavRecorder &recorder, Print &log) {
  std::array<int16_t, 320> samples{};
  for (size_t index = 0; index < samples.size(); ++index) {
    samples[index] = static_cast<int16_t>((index * 41U) & 0x3fffU);
  }
  assert(recorder.appendMono16(samples.data(), samples.size(), log));
}

void drive(WavRecorder &recorder,
           const std::shared_ptr<fakefs::State> &state,
           Print &log, uint32_t nowMs,
           CapsuleTransactionGate *gate = nullptr) {
  uint32_t polls = 0;
  while (recorder.operationActive() && polls++ < 30000U) {
    const uint32_t before = state->operations;
    (void)recorder.pollFinalize(log, nowMs, gate);
    assert(state->operations - before <= 1U);
  }
  assert(polls < 30000U);
  assert(!recorder.operationActive());
  assert(state->openHandles == 0);
  assert(state->maximumReadBytes <= kCapsuleTransactionIoBytes);
  assert(state->maximumWriteBytes <= kCapsuleTransactionIoBytes);
}

StoredRecorderCheckpoint failedCheckpoint(
    const std::shared_ptr<fakefs::State> &state, const char *id) {
  std::string path = std::string(kCapsuleStaging) + "/" + id +
      "/recording.failed.chk";
  auto found = state->files.find(path);
  if (found == state->files.end()) {
    path = std::string(kCapsuleStaging) + "/" + id +
        "/recording.failure";
    found = state->files.find(path);
  }
  assert(found != state->files.end());
  assert(found->second.size() == sizeof(StoredRecorderCheckpoint));
  StoredRecorderCheckpoint checkpoint{};
  memcpy(&checkpoint, found->second.data(), sizeof(checkpoint));
  assert(validateRecorderCheckpoint(checkpoint));
  assert(checkpoint.state ==
         static_cast<uint8_t>(RecorderCheckpointState::failed));
  return checkpoint;
}

void runNormalPendingAndSecondRecording() {
  QuietPrint log;
  auto state = std::make_shared<fakefs::State>();
  fs::FS storage(state);
  WavRecorder recorder;
  assert(recorder.begin(storage, log));
  finishBootRecovery(recorder, state, log);
  assert(recorder.start(log, kFirstId, kCreatedAt, admittedSpace(),
                        RecorderOperationOwner::localApp));
  appendFrame(recorder, log);
  const uint32_t operationsBeforeStop = state->operations;
  assert(recorder.stop(log, RecorderStopReason::user));
  assert(state->operations == operationsBeforeStop);
  assert(recorder.finalizing());
  assert(recorder.ownedBy(RecorderOperationOwner::localApp));
  assert(!recorder.terminalResult().pending());
  assert(!recorder.start(log, kSecondId, kCreatedAt, admittedSpace(),
                         RecorderOperationOwner::localApp));

  drive(recorder, state, log, 700000U, nullptr);
  assert(recorder.terminalResult().success());
  const std::string inbox = std::string(kCapsuleInbox) + "/" + kFirstId;
  assert(state->files.count(inbox + "/audio.wav") == 1);
  assert(state->text(inbox + "/processing.json").find("queued") !=
         std::string::npos);
  assert(state->text(inbox + "/capsule.json").find(kFirstId) !=
         std::string::npos);
  assert(state->files.count(inbox + "/recording.chk") == 0);
  assert(recorder.operationOwner() == RecorderOperationOwner::none);
  RecorderOutcome acknowledged;
  assert(recorder.takeTerminalResult(acknowledged));

  assert(recorder.start(log, kSecondId, kCreatedAt, admittedSpace(),
                        RecorderOperationOwner::localApp));
  appendFrame(recorder, log);
  assert(recorder.stop(log));
  drive(recorder, state, log, 800000U, nullptr);
  assert(recorder.terminalResult().success());
}

void runStorageContextOwnsReservationAndFile() {
  QuietPrint log;
  auto state = std::make_shared<fakefs::State>();
  fs::FS storage(state);
  WavRecorder recorder;
  assert(recorder.begin(storage, log));
  finishBootRecovery(recorder, state, log);

  std::atomic<bool> finished{false};
  std::thread storageContext([&]() {
    for (const char *id : {kFirstId, kSecondId}) {
      assert(recorder.start(log, id, kCreatedAt, admittedSpace(),
                            RecorderOperationOwner::localApp));
      appendFrame(recorder, log);
      assert(recorder.stop(log));
      drive(recorder, state, log, 700000U, nullptr);
      assert(recorder.terminalResult().success());
      RecorderOutcome acknowledged;
      assert(recorder.takeTerminalResult(acknowledged));
      assert(StorageCoordinator::instance().idle());
      assert(state->openHandles == 0U);
    }
    finished.store(true, std::memory_order_release);
  });
  uint32_t uiTicks = 0;
  while (!finished.load(std::memory_order_acquire)) {
    ++uiTicks;
    std::this_thread::yield();
  }
  storageContext.join();
  assert(uiTicks > 0U);
  assert(state->files.count(std::string(kCapsuleInbox) + "/" + kFirstId +
                            "/audio.wav") == 1U);
  assert(state->files.count(std::string(kCapsuleInbox) + "/" + kSecondId +
                            "/audio.wav") == 1U);
}

void runCaptureAbortCannotPublishQueued() {
  QuietPrint log;
  auto state = std::make_shared<fakefs::State>();
  fs::FS storage(state);
  WavRecorder recorder;
  assert(recorder.begin(storage, log));
  finishBootRecovery(recorder, state, log);
  assert(recorder.start(log, kFirstId, kCreatedAt, admittedSpace(),
                        RecorderOperationOwner::localApp));
  appendFrame(recorder, log);
  assert(recorder.abortCapture(log));
  drive(recorder, state, log, 5000U, nullptr);
  assert(recorder.terminalResult().terminal ==
         RecorderTerminal::captureFailure);
  assert(state->directories.count(std::string(kCapsuleInbox) + "/" +
                                  kFirstId) == 0U);
  const std::string failed = std::string(kCapsuleStaging) + "/" + kFirstId;
  assert(state->files.count(failed + "/audio.wav.part") == 1U);
  assert(failedCheckpoint(state, kFirstId).failureStage ==
         static_cast<uint8_t>(RecorderFailureStage::captureIncomplete));
  RecorderOutcome acknowledged;
  assert(recorder.takeTerminalResult(acknowledged));

  assert(recorder.start(log, kSecondId, kCreatedAt, admittedSpace(),
                        RecorderOperationOwner::localApp));
  appendFrame(recorder, log);
  assert(recorder.stop(log));
  drive(recorder, state, log, 6000U, nullptr);
  assert(recorder.terminalResult().success());
}

void runAbsoluteGateAndUsbNullGate() {
  QuietPrint log;
  auto state = std::make_shared<fakefs::State>();
  fs::FS storage(state);
  WavRecorder wifi;
  assert(wifi.begin(storage, log));
  finishBootRecovery(wifi, state, log);
  assert(wifi.start(log, kFirstId, kCreatedAt, admittedSpace(),
                    RecorderOperationOwner::linkWifi));
  appendFrame(wifi, log);
  AbsoluteCapsuleTransactionDeadlineGate gate;
  gate.arm(0, 300000U);
  assert(wifi.stop(log));
  for (uint8_t phase = 0; phase < 7; ++phase) {
    (void)wifi.pollFinalize(log, 299000U, &gate);
  }
  assert(wifi.operationActive());
  drive(wifi, state, log, 300000U, &gate);
  assert(wifi.terminalResult().terminal == RecorderTerminal::cancelled);
  const StoredRecorderCheckpoint cancelled = failedCheckpoint(state, kFirstId);
  assert(cancelled.failureStage ==
         static_cast<uint8_t>(RecorderFailureStage::commitAudio));
  const std::string staging = std::string(kCapsuleStaging) + "/" + kFirstId;
  assert(state->files.count(staging + "/audio.wav.part") == 1);
  assert(state->text(staging + "/processing.json").find("queued") ==
         std::string::npos);

  WavRecorder usb;
  assert(usb.begin(storage, log));
  finishBootRecovery(usb, state, log);
  assert(usb.start(log, kSecondId, kCreatedAt, admittedSpace(),
                   RecorderOperationOwner::linkUsb));
  appendFrame(usb, log);
  assert(usb.stop(log));
  drive(usb, state, log, 900000U, nullptr);
  assert(usb.terminalResult().success());
}

void runFailureCheckpointAndRecoveryTruth() {
  for (const auto fault : {
           fakefs::Operation::flush,
           fakefs::Operation::seek,
           fakefs::Operation::close,
       }) {
    QuietPrint log;
    auto state = std::make_shared<fakefs::State>();
    fs::FS storage(state);
    WavRecorder recorder;
    assert(recorder.begin(storage, log));
    finishBootRecovery(recorder, state, log);
    assert(recorder.start(log, kFirstId, kCreatedAt, admittedSpace(),
                          RecorderOperationOwner::localApp));
    appendFrame(recorder, log);
    state->fail(fault, 1, fakefs::FaultAction::returnFailure);
    assert(recorder.stop(log));
    drive(recorder, state, log, 0, nullptr);
    const StoredRecorderCheckpoint checkpoint =
        failedCheckpoint(state, kFirstId);
    assert(strcmp(checkpoint.createdAt, kCreatedAt) == 0);
    assert(checkpoint.confirmedDataBytes == 640U);
    assert(checkpoint.failureStage != 0);
    const std::string staging =
        std::string(kCapsuleStaging) + "/" + kFirstId;
    assert(state->directories.count(staging) == 1);
    assert(state->directories.count(std::string(kCapsuleInbox) + "/" +
                                    kFirstId) == 0);

    WavRecorder rebooted;
    assert(rebooted.begin(storage, log));
    finishBootRecovery(rebooted, state, log);
    assert(state->directories.count(staging) == 1);
    assert(failedCheckpoint(state, kFirstId).failureStage ==
           checkpoint.failureStage);

    WavRecorder rebootedAgain;
    assert(rebootedAgain.begin(storage, log));
    finishBootRecovery(rebootedAgain, state, log);
    assert(state->directories.count(staging) == 1);
    assert(failedCheckpoint(state, kFirstId).failureStage ==
           checkpoint.failureStage);
  }
}

void seedInterruptedRecording(const std::shared_ptr<fakefs::State> &state,
                              const char *id, uint32_t dataBytes) {
  const std::string directory = std::string(kCapsuleStaging) + "/" + id;
  std::vector<uint8_t> wav(kWavHeaderBytes + dataBytes);
  encodeWavHeader(wav.data(), 0);
  uint32_t crc = recorderAudioCrc32Begin();
  for (uint32_t offset = 0; offset < dataBytes; ++offset) {
    wav[kWavHeaderBytes + offset] = static_cast<uint8_t>(offset * 17U + 3U);
  }
  crc = recorderAudioCrc32Update(
      crc, wav.data() + kWavHeaderBytes, dataBytes);

  StoredRecorderCheckpoint checkpoint{};
  assert(initializeRecorderCheckpoint(checkpoint, id, kCreatedAt));
  updateRecorderCheckpoint(
      checkpoint, dataBytes, recorderAudioCrc32Finish(crc));
  state->directories.insert(directory);
  state->seedBytes(directory + "/audio.wav.part", wav.data(), wav.size());
  state->seedBytes(directory + "/recording.chk",
                   reinterpret_cast<const uint8_t *>(&checkpoint),
                   sizeof(checkpoint));
}

void runIncrementalBootRecoveryBudget() {
  QuietPrint log;
  auto state = std::make_shared<fakefs::State>();
  fs::FS storage(state);
  seedInterruptedRecording(state, kFirstId, kMaximumRecordingAudioBytes);
  seedInterruptedRecording(state, kSecondId, 4096U);

  WavRecorder recorder;
  assert(recorder.begin(storage, log));
  assert(recorder.recoveryPending());
  assert(!recorder.start(log, "fedcba98-7654-4abc-8def-1234567890ab",
                         kCreatedAt, admittedSpace(),
                         RecorderOperationOwner::localApp));
  uint32_t polls = 0;
  uint32_t uiHeartbeats = 0;
  while (recorder.recoveryPending() && polls++ < 30000U) {
    const uint32_t before = state->operations;
    (void)recorder.pollFinalize(log, polls, nullptr);
    assert(state->operations - before <= 1U);
    ++uiHeartbeats;
  }
  assert(polls < 30000U);
  assert(uiHeartbeats > 3000U);
  assert(recorder.takeRecoveryReady());
  assert(!recorder.recoveryFailed());
  assert(state->openHandles == 0);
  assert(state->maximumReadBytes <= kCapsuleTransactionIoBytes);
  assert(state->maximumWriteBytes <= kCapsuleTransactionIoBytes);
  assert(state->files.count(std::string(kCapsuleInbox) + "/" + kFirstId +
                            "/audio.wav") == 1);
  assert(state->files.count(std::string(kCapsuleInbox) + "/" + kSecondId +
                            "/audio.wav") == 1);

  WavRecorder rebooted;
  assert(rebooted.begin(storage, log));
  finishBootRecovery(rebooted, state, log);
  assert(state->openHandles == 0);
}

void runBootRecoveryFailClosed() {
  enum class Corruption : uint8_t {
    version,
    checkpointCrc,
    directoryBinding,
    confirmedLength,
    audioCrc,
    missingCheckpoint,
  };
  for (const Corruption corruption : {
           Corruption::version,
           Corruption::checkpointCrc,
           Corruption::directoryBinding,
           Corruption::confirmedLength,
           Corruption::audioCrc,
           Corruption::missingCheckpoint,
       }) {
    QuietPrint log;
    auto state = std::make_shared<fakefs::State>();
    fs::FS storage(state);
    seedInterruptedRecording(state, kFirstId, 4096U);
    const std::string checkpointPath = std::string(kCapsuleStaging) + "/" +
        kFirstId + "/recording.chk";
    if (corruption == Corruption::missingCheckpoint) {
      state->files.erase(checkpointPath);
    } else {
      StoredRecorderCheckpoint checkpoint{};
      memcpy(&checkpoint, state->files[checkpointPath].data(),
             sizeof(checkpoint));
      if (corruption == Corruption::version) {
        ++checkpoint.version;
        finalizeRecorderCheckpoint(checkpoint);
      } else if (corruption == Corruption::checkpointCrc) {
        checkpoint.crc32 ^= 0x100U;
      } else if (corruption == Corruption::directoryBinding) {
        assert(recorderCheckpointCopy(checkpoint.capsuleId,
                                      sizeof(checkpoint.capsuleId),
                                      kSecondId));
        finalizeRecorderCheckpoint(checkpoint);
      } else if (corruption == Corruption::confirmedLength) {
        updateRecorderCheckpoint(checkpoint, 8192U, checkpoint.audioCrc32);
      } else if (corruption == Corruption::audioCrc) {
        updateRecorderCheckpoint(checkpoint, checkpoint.confirmedDataBytes,
                                 checkpoint.audioCrc32 ^ 0x80000000U);
      }
      state->seedBytes(checkpointPath,
                       reinterpret_cast<const uint8_t *>(&checkpoint),
                       sizeof(checkpoint));
    }

    WavRecorder recorder;
    assert(recorder.begin(storage, log));
    uint32_t polls = 0;
    while (recorder.recoveryPending() && polls++ < 10000U) {
      const uint32_t before = state->operations;
      (void)recorder.pollFinalize(log, polls, nullptr);
      assert(state->operations - before <= 1U);
    }
    assert(polls < 10000U);
    assert(!recorder.recoveryFailed());
    assert(recorder.takeRecoveryReady());
    assert(!recorder.operationActive());
    assert(state->openHandles == 0);
    assert(StorageCoordinator::instance().idle());
    assert(state->directories.count(std::string(kCapsuleStaging) + "/" +
                                    kFirstId + ".blocked") == 1);
    assert(recorder.start(log, kSecondId, kCreatedAt, admittedSpace(),
                          RecorderOperationOwner::localApp));
    assert(recorder.abortCapture(log));
    drive(recorder, state, log, 12000U, nullptr);
    RecorderOutcome acknowledged;
    assert(recorder.takeTerminalResult(acknowledged));
  }
}

void runCommittedAudioRecoveryPhase() {
  QuietPrint log;
  auto state = std::make_shared<fakefs::State>();
  fs::FS storage(state);
  constexpr uint32_t kBytes = 8192U;
  seedInterruptedRecording(state, kFirstId, kBytes);
  const std::string directory = std::string(kCapsuleStaging) + "/" + kFirstId;
  std::vector<uint8_t> audio = state->files[directory + "/audio.wav.part"];
  encodeWavHeader(audio.data(), kBytes);
  state->files.erase(directory + "/audio.wav.part");
  state->seedBytes(directory + "/audio.wav", audio.data(), audio.size());
  state->seed(directory + "/processing.json",
              "{\"status\":\"queued\",\"revision\":2}");

  WavRecorder recorder;
  assert(recorder.begin(storage, log));
  finishBootRecovery(recorder, state, log);
  const std::string inbox = std::string(kCapsuleInbox) + "/" + kFirstId;
  assert(state->files.count(inbox + "/audio.wav") == 1);
  assert(state->files.count(inbox + "/capsule.json") == 1);
  assert(state->text(inbox + "/processing.json").find("queued") !=
         std::string::npos);
  assert(state->directories.count(directory) == 0);

  WavRecorder rebooted;
  assert(rebooted.begin(storage, log));
  finishBootRecovery(rebooted, state, log);
  assert(state->files.count(inbox + "/audio.wav") == 1);
}

void runBadFirstStagingDoesNotBlockHealthyRecovery() {
  for (const bool persistentOpenFailure : {false, true}) {
    QuietPrint log;
    auto state = std::make_shared<fakefs::State>();
    fs::FS storage(state);
    seedInterruptedRecording(state, kFirstId, 4096U);
    seedInterruptedRecording(state, kSecondId, 4096U);
    const std::string third = std::string(kCapsuleStaging) + "/" + kThirdId;
    StoredRecorderCheckpoint preserved{};
    assert(initializeRecorderCheckpoint(preserved, kThirdId, kCreatedAt));
    failRecorderCheckpoint(preserved, RecorderFailureStage::shortWrite);
    state->directories.insert(third);
    state->seedBytes(third + "/recording.failed.chk",
                     reinterpret_cast<const uint8_t *>(&preserved),
                     sizeof(preserved));
    const std::string first = std::string(kCapsuleStaging) + "/" + kFirstId;
    if (!persistentOpenFailure) {
      state->seed(first + "/recording.chk", "wrong-size");
    }

    WavRecorder recorder;
    assert(recorder.begin(storage, log));
    bool faultInjected = !persistentOpenFailure;
    bool faultCleared = !persistentOpenFailure;
    uint32_t polls = 0;
    while (recorder.recoveryPending() && polls++ < 20000U) {
      if (persistentOpenFailure && !faultInjected &&
          state->openNextFileCalls >= 1U && state->openHandles == 1U) {
        state->failAlways(fakefs::Operation::open,
                          fakefs::FaultAction::returnFailure);
        faultInjected = true;
      }
      if (persistentOpenFailure && faultInjected && !faultCleared &&
          state->directories.count(first + ".blocked") == 1U) {
        state->clearFault();
        faultCleared = true;
      }
      const uint32_t before = state->operations;
      (void)recorder.pollFinalize(log, polls, nullptr);
      assert(state->operations - before <= 1U);
    }
    assert(polls < 20000U);
    assert(faultInjected && faultCleared);
    assert(recorder.takeRecoveryReady());
    assert(state->directories.count(first + ".blocked") == 1U);
    assert(state->files.count(std::string(kCapsuleInbox) + "/" + kSecondId +
                              "/audio.wav") == 1U);
    assert(state->directories.count(third) == 1U);
    assert(state->files.count(third + "/recording.failed.chk") == 1U);
    assert(state->openNextFileCalls == 3U);
    assert(state->openHandles == 0U);
  }
}

void runPatchFailureIsolatesOnlyCandidate() {
  for (const fakefs::Operation operation : {
           fakefs::Operation::seek,
           fakefs::Operation::write,
           fakefs::Operation::flush,
           fakefs::Operation::close,
       }) {
    QuietPrint log;
    auto state = std::make_shared<fakefs::State>();
    fs::FS storage(state);
    seedInterruptedRecording(state, kFirstId, 4096U);
    seedInterruptedRecording(state, kSecondId, 4096U);
    const std::string first = std::string(kCapsuleStaging) + "/" + kFirstId;
    const std::string firstPartial = first + "/audio.wav.part";
    WavRecorder recorder;
    assert(recorder.begin(storage, log));
    bool injected = false;
    bool cleared = false;
    uint32_t polls = 0;
    while (recorder.recoveryPending() && polls++ < 20000U) {
      if (!injected && state->lastOpenedPath == firstPartial &&
          state->lastOpenWritable && state->openHandles >= 2U) {
        state->failAlways(operation, fakefs::FaultAction::returnFailure);
        injected = true;
      }
      if (injected && !cleared &&
          state->directories.count(first + ".blocked") == 1U) {
        state->clearFault();
        cleared = true;
      }
      const uint32_t before = state->operations;
      (void)recorder.pollFinalize(log, polls, nullptr);
      assert(state->operations - before <= 1U);
    }
    assert(polls < 20000U);
    assert(injected && cleared);
    assert(recorder.takeRecoveryReady());
    assert(state->directories.count(first + ".blocked") == 1U);
    assert(state->files.count(std::string(kCapsuleInbox) + "/" + kSecondId +
                              "/audio.wav") == 1U);
    assert(state->openHandles == 0U);
  }
}

void runFailureFactMustBecomeDurable() {
  QuietPrint log;
  auto state = std::make_shared<fakefs::State>();
  fs::FS storage(state);
  WavRecorder recorder;
  assert(recorder.begin(storage, log));
  finishBootRecovery(recorder, state, log);
  assert(recorder.start(log, kFirstId, kCreatedAt, admittedSpace(),
                        RecorderOperationOwner::localApp));
  appendFrame(recorder, log);
  assert(recorder.stop(log));
  state->failAlways(fakefs::Operation::write,
                    fakefs::FaultAction::returnFailure);
  state->failAlsoAlways(fakefs::Operation::rename,
                        fakefs::FaultAction::returnFailure);
  for (uint32_t poll = 0; recorder.operationActive() && poll < 2000U; ++poll) {
    const uint32_t before = state->operations;
    (void)recorder.pollFinalize(log, poll, nullptr);
    assert(state->operations - before <= 1U);
  }
  assert(!recorder.operationActive());
  assert(!recorder.cleanupPending());
  assert(recorder.terminalResult().terminal ==
         RecorderTerminal::cleanupBlocked);
  assert(recorder.recoveryFailed());
  assert(recorder.operationOwner() == RecorderOperationOwner::none);
  assert(StorageCoordinator::instance().idle());
  assert(state->openHandles == 0U);
  const std::string staging = std::string(kCapsuleStaging) + "/" + kFirstId;
  assert(state->directories.count(staging) == 1U);
  assert(state->files.count(staging + "/audio.wav.part") == 1U);
  assert(!recorder.start(log, kSecondId, kCreatedAt, admittedSpace(),
                         RecorderOperationOwner::localApp));
  RecorderOutcome acknowledged;
  assert(recorder.takeTerminalResult(acknowledged));
  assert(!recorder.start(log, kSecondId, kCreatedAt, admittedSpace(),
                         RecorderOperationOwner::localApp));
}

void runRecoveryFinalizeFailureDoesNotWedge() {
  QuietPrint log;
  auto state = std::make_shared<fakefs::State>();
  fs::FS storage(state);
  seedInterruptedRecording(state, kFirstId, 4096U);
  WavRecorder recorder;
  assert(recorder.begin(storage, log));

  bool injected = false;
  uint32_t polls = 0;
  while (recorder.recoveryPending() && polls++ < 20000U) {
    if (!injected) {
      for (const auto &entry : state->files) {
        if (entry.first.find("/.system/transactions/") != std::string::npos &&
            entry.first.find(".journal") != std::string::npos) {
          state->failAlways(fakefs::Operation::rename,
                            fakefs::FaultAction::returnFailure);
          injected = true;
          break;
        }
      }
    }
    const uint32_t before = state->operations;
    (void)recorder.pollFinalize(log, polls, nullptr);
    assert(state->operations - before <= 1U);
  }
  assert(injected);
  assert(polls < 20000U);
  assert(!recorder.recoveryPending());
  assert(recorder.takeRecoveryReady() || recorder.recoveryFailed());
  assert(!recorder.operationActive());
  assert(state->openHandles == 0);
  assert(StorageCoordinator::instance().idle());
}

void runAutomaticMaximumDuration() {
  QuietPrint log;
  auto state = std::make_shared<fakefs::State>();
  fs::FS storage(state);
  WavRecorder recorder;
  assert(recorder.begin(storage, log));
  finishBootRecovery(recorder, state, log);
  assert(recorder.start(log, kFirstId, kCreatedAt, admittedSpace(),
                        RecorderOperationOwner::localApp));
  std::array<int16_t, 2048> samples{};
  uint32_t frames = 0;
  while (!recorder.stopRequested() && frames++ < 1000U) {
    assert(recorder.appendMono16(samples.data(), samples.size(), log));
  }
  assert(frames < 1000U);
  assert(recorder.recording());
  assert(recorder.requestedStopReason() == RecorderStopReason::maxDuration);
  assert(recorder.durationMs() == kMaxRecordingMs);
  assert(recorder.stop(log, recorder.requestedStopReason()));
  drive(recorder, state, log, 1000000U, nullptr);
  assert(recorder.terminalResult().success());
  assert(recorder.terminalResult().stopReason ==
         RecorderStopReason::maxDuration);
  assert(recorder.terminalResult().dataBytes ==
         kMaximumRecordingAudioBytes);
}

}  // namespace

int main() {
  runNormalPendingAndSecondRecording();
  runStorageContextOwnsReservationAndFile();
  runCaptureAbortCannotPublishQueued();
  runAbsoluteGateAndUsbNullGate();
  runFailureCheckpointAndRecoveryTruth();
  runIncrementalBootRecoveryBudget();
  runBootRecoveryFailClosed();
  runCommittedAudioRecoveryPhase();
  runBadFirstStagingDoesNotBlockHealthyRecovery();
  runPatchFailureIsolatesOnlyCandidate();
  runFailureFactMustBecomeDurable();
  runRecoveryFinalizeFailureDoesNotWedge();
  runAutomaticMaximumDuration();
  return 0;
}
