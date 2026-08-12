#pragma once

#include <Arduino.h>
#include <FS.h>
#include <atomic>
#if defined(ARDUINO_ARCH_ESP32)
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif

#include "AudioFrontEnd.h"
#include "BoardConfig.h"
#include "CapsuleTransaction.h"
#include "RecorderCheckpoint.h"
#include "RecorderOutcome.h"
#include "RecorderStartState.h"
#include "RecorderStorageQueue.h"
#include "RecordingAdmissionPolicy.h"
#include "StorageCoordinator.h"

namespace pokepod {

class WavRecorder {
 public:
  bool begin(fs::FS &fs, Print &log);
  bool start(Print &log, const String &recordingId, const String &createdAt);
  bool start(Print &log, const String &recordingId, const String &createdAt,
             const RecordingSpaceSnapshot &space);
  bool start(Print &log, const String &recordingId, const String &createdAt,
             const RecordingSpaceSnapshot &space,
             RecorderOperationOwner owner);
  bool requestStart(Print &log, const String &recordingId,
                    const String &createdAt,
                    const RecordingSpaceSnapshot &space,
                    RecorderOperationOwner owner);
  RecorderStartPollResult pollStart(
      Print &log, uint32_t nowMs,
      CapsuleTransactionGate *gate = nullptr);
  bool appendMono16(const int16_t *samples, size_t sampleCount, Print &log);
  bool stop(Print &log,
            RecorderStopReason reason = RecorderStopReason::user);
  bool abortCapture(Print &log);
  // Latches the first capture-delivery failure and asks the storage owner to
  // create a durable failed terminal. This is intentionally public only for
  // the single AudioCaptureDispatcher; callers cannot clear or downgrade it.
  void reportCaptureFailure(Print &log, RecorderTerminal terminal,
                            RecorderFailureStage stage);
  bool captureFailureLatched() const {
    return captureFailureCode_.load(std::memory_order_acquire) != 0U;
  }
  RecorderFailureStage captureFailureStage() const {
    return static_cast<RecorderFailureStage>(
        captureFailureCode_.load(std::memory_order_acquire) & 0xFFU);
  }
  RecorderTerminal captureFailureTerminal() const {
    return static_cast<RecorderTerminal>(
        (captureFailureCode_.load(std::memory_order_acquire) >> 8U) & 0xFFU);
  }
  // Advances at most one recorder primitive or one bounded transaction poll.
  // A null gate is the local/USB policy: no transfer-window deadline.
  bool pollFinalize(Print &log, uint32_t nowMs = 0,
                    CapsuleTransactionGate *gate = nullptr);
  // Failure cleanup can be deferred while another storage context owns the
  // physical SD bus.  The application polls this once per loop; until it
  // completes the recorder keeps its mutation reservation and rejects reuse.
  bool pollCleanup(Print &log, uint32_t nowMs = 0,
                   CapsuleTransactionGate *gate = nullptr);
  bool cleanupPending() const { return cleanupPending_; }
  bool recoveryPending() const { return bootRecoveryPending_; }
  bool recoveryFailed() const { return bootRecoveryFailed_; }
  bool takeRecoveryReady() {
    const bool ready = bootRecoveryReady_;
    bootRecoveryReady_ = false;
    return ready;
  }

  bool recording() const { return recording_; }
  bool finalizing() const { return finalizePending_; }
  bool operationActive() const {
#if defined(ARDUINO_ARCH_ESP32)
    // UI/power code reads only atomics shared by the storage task. Runner and
    // checkpoint phases remain private to their owning task.
    return recording_ || automaticStopRequested_ || finalizePending_ ||
        cleanupPending_ || bootRecoveryPending_ ||
        storageSessionActive_.load(std::memory_order_acquire) ||
        storageStartRequested_.load(std::memory_order_acquire)
        ;
#else
    return recording_ || automaticStopRequested_ || finalizePending_ ||
        cleanupPending_ || bootRecoveryPending_ || transactionRunner_.active();
#endif
  }
  bool stopRequested() const { return automaticStopRequested_; }
  RecorderStopReason requestedStopReason() const {
    return automaticStopReason_.load(std::memory_order_acquire);
  }
  RecorderOperationOwner operationOwner() const { return operationOwner_; }
  bool ownedBy(RecorderOperationOwner owner) const {
    return operationOwner_ == owner;
  }
  uint32_t durationMs() const;
  const String &finalPath() const { return finalPath_; }
  const String &capsuleId() const { return recordingId_; }
  const AudioFrontEndMetrics &audioMetrics() const {
    return audioMetrics_;
  }
  void observeAudioMetrics(uint32_t sessionId, uint32_t generation,
                           const AudioFrontEndMetrics &metrics) {
    if (sessionId == 0 || generation == 0) return;
    if (audioMetricsSessionId_ != sessionId) {
      audioMetricsSessionId_ = sessionId;
      audioMetricsGeneration_ = 0;
      audioMetrics_ = {};
    }
    if (generation < audioMetricsGeneration_) return;
    audioMetricsGeneration_ = generation;
    audioMetrics_ = metrics;
  }
  uint32_t audioMetricsSessionId() const { return audioMetricsSessionId_; }
  uint32_t audioMetricsGeneration() const { return audioMetricsGeneration_; }
  const RecorderOutcome &terminalResult() const {
    return terminalState_.peek();
  }
  bool takeTerminalResult(RecorderOutcome &outcome) {
    return terminalState_.take(outcome);
  }

 private:
  bool startInternal(Print &log, const String &recordingId,
                     const String &createdAt,
                     const RecordingSpaceSnapshot *space,
                     RecorderOperationOwner owner);
  bool requestStartInternal(Print &log, const String &recordingId,
                            const String &createdAt,
                            const RecordingSpaceSnapshot *space,
                            RecorderOperationOwner owner);
  void resetSessionState();
  bool finishFailure(Print &log, RecorderTerminal terminal,
                     RecorderFailureStage stage);
  bool ensureDirectory(const char *path, Print &log);
  bool writeTextAtomically(const String &finalPath, const String &text, Print &log);
  bool writeHeader(uint32_t dataBytes);
  bool writeProcessingMetadata(Print &log, const char *status, uint32_t revision,
                               const char *errorStage, const char *error);
  bool persistCheckpoint(bool failed, RecorderFailureStage stage,
                         Print &log);
  bool appendMonoBytes(const uint8_t *data, size_t length, Print &log);
  bool storageAppendMonoBytes(const uint8_t *data, size_t length, Print &log);
  bool startStorageSession(Print &log);
  StorageOwner activeStorageOwner() const;
  bool pollFinalizeRunner(Print &log, uint32_t nowMs,
                          CapsuleTransactionGate *gate);
  bool finalizeFailure(Print &log, RecorderTerminal terminal,
                       RecorderFailureStage stage);
  void completeFinalize(Print &log);
  bool pollBootRecovery(Print &log, uint32_t nowMs);
  bool pollStorage(Print &log, uint32_t nowMs,
                   CapsuleTransactionGate *gate);

#if defined(ARDUINO_ARCH_ESP32)
  static void storageTaskThunk(void *context);
  void storageTaskMain();
  bool ensureStorageTask(Print &log);
  bool pollPeriodicCheckpoint(Print &log, uint32_t nowMs,
                              CapsuleTransactionGate *gate);
#endif

  class StringByteSource final : public CapsuleTransactionByteSource {
   public:
    void bind(const String &value) { value_ = &value; }
    uint32_t length() const override {
      return value_ == nullptr ? 0U : static_cast<uint32_t>(value_->length());
    }
    size_t readAt(uint32_t offset, uint8_t *destination,
                  size_t maximumBytes) override {
      if (value_ == nullptr || destination == nullptr ||
          offset >= value_->length()) return 0;
      size_t count = value_->length() - offset;
      if (count > maximumBytes) count = maximumBytes;
      memcpy(destination, value_->c_str() + offset, count);
      return count;
    }

   private:
    const String *value_ = nullptr;
  };

  class CheckpointByteSource final : public CapsuleTransactionByteSource {
   public:
    void bind(const StoredRecorderCheckpoint &checkpoint) {
      checkpoint_ = &checkpoint;
    }
    uint32_t length() const override {
      return checkpoint_ == nullptr ? 0U : sizeof(*checkpoint_);
    }
    size_t readAt(uint32_t offset, uint8_t *destination,
                  size_t maximumBytes) override {
      if (checkpoint_ == nullptr || destination == nullptr ||
          offset >= sizeof(*checkpoint_)) return 0;
      size_t count = sizeof(*checkpoint_) - offset;
      if (count > maximumBytes) count = maximumBytes;
      memcpy(destination,
             reinterpret_cast<const uint8_t *>(checkpoint_) + offset, count);
      return count;
    }

   private:
    const StoredRecorderCheckpoint *checkpoint_ = nullptr;
  };

  enum class FinalizePhase : uint8_t {
    idle = 0,
    flushAudio,
    seekHeader,
    writeHeader,
    finalFlush,
    closeAudio,
    startAudioAndProcessing,
    pollAudioAndProcessing,
    startCapsuleMetadata,
    pollCapsuleMetadata,
    checkInboxDirectory,
    renameStagingDirectory,
    checkCheckpoint,
    removeCheckpoint,
    quarantineCheckpoint,
    complete,
  };

  enum class CleanupPhase : uint8_t {
    idle = 0,
    flushAudio,
    closeAudio,
    removeEmptyStaging,
    startFailureCheckpoint,
    pollFailureCheckpoint,
    openFailureMarker,
    writeFailureMarker,
    flushFailureMarker,
    closeFailureMarker,
    openFailureMarkerReadback,
    readFailureMarker,
    closeFailureMarkerReadback,
    renameFailureCheckpoint,
    publishTerminal,
  };

  enum class RecoveryPhase : uint8_t {
    transaction = 0,
    reserve,
    scanOpen,
    scanNext,
    closeEntry,
    closeRoot,
    checkFailureMarker,
    checkFailedCheckpoint,
    checkQuarantinedCheckpoint,
    checkCheckpoint,
    checkOrphanPartial,
    openCheckpoint,
    readCheckpoint,
    closeCheckpoint,
    checkCommittedAudio,
    openPartial,
    readCommittedHeader,
    seekPartial,
    readPartialCrc,
    closePartial,
    truncatePartial,
    openPatch,
    seekPatch,
    writePatch,
    flushPatch,
    closePatch,
    startFinalize,
    quarantineCloseFile,
    quarantineDestinationExists,
    quarantineRename,
    failureCloseFile,
    failureCloseEntry,
    failureCloseRoot,
    failureDone,
    done,
    failed,
  };

  enum class PeriodicCheckpointPhase : uint8_t {
    idle = 0,
    flushAudio,
    startTransaction,
    pollTransaction,
  };

  fs::FS *fs_ = nullptr;
  File file_;
  File recoveryRoot_;
  File recoveryEntry_;
  String recordingId_;
  String createdAt_;
  String directory_;
  String partialPath_;
  String finalPath_;
  uint32_t dataBytes_ = 0;
  std::atomic<bool> recording_{false};
  // First failure wins across the UI/capture producer and the storage task.
  // A non-none stage is an irreversible "this recording cannot commit" fact.
  // Terminal and stage are published as one atomic fact. A packed value avoids
  // readers observing a failure stage before its terminal (or vice versa).
  std::atomic<uint16_t> captureFailureCode_{0};
  std::atomic<RecorderOperationOwner> operationOwner_{
      RecorderOperationOwner::none};
  bool checkpointInitialized_ = false;
  uint32_t checkpointCrcState_ = recorderAudioCrc32Begin();
  uint32_t checkpointedBytes_ = 0;
  StoredRecorderCheckpoint checkpoint_{};
  StorageReservation storageReservation_;
  // Capture admission writes only bounded processing/checkpoint metadata with
  // this synchronous helper. WAV facts, CRC, publish and boot recovery use the
  // cooperative runner exclusively.
  CapsuleTransaction transaction_;
  CapsuleTransactionRunner transactionRunner_;
  CapsuleTransactionInput finalizeInputs_[2]{};
  String processingMetadata_;
  String capsuleMetadata_;
  String inboxDirectory_;
  String failureMarkerPath_;
  StringByteSource processingSource_;
  StringByteSource capsuleSource_;
  CheckpointByteSource failureCheckpointSource_;
  StoredRecorderCheckpoint failureMarkerReadback_{};
  bool failureMarkerReadbackValid_ = false;
  RecorderOutcomeState terminalState_;
  // Compatibility snapshot for status surfaces that still obtain recorder
  // diagnostics. The capture runtime owns all stereo DSP and the application
  // injects only coherent, generation-bound POD snapshots here.
  AudioFrontEndMetrics audioMetrics_{};
  uint32_t audioMetricsSessionId_ = 0;
  uint32_t audioMetricsGeneration_ = 0;
  FinalizePhase finalizePhase_ = FinalizePhase::idle;
  std::atomic<bool> finalizePending_{false};
  RecorderStopReason finalizeStopReason_ = RecorderStopReason::none;
  uint8_t finalizePrimitiveFailures_ = 0;
  std::atomic<bool> automaticStopRequested_{false};
  std::atomic<RecorderStopReason> automaticStopReason_{
      RecorderStopReason::none};
  std::atomic<bool> cleanupPending_{false};
  CleanupPhase cleanupPhase_ = CleanupPhase::idle;
  uint8_t cleanupPrimitiveFailures_ = 0;
  RecorderTerminal cleanupTerminal_ = RecorderTerminal::none;
  RecorderFailureStage cleanupStage_ = RecorderFailureStage::none;
  bool bootRecoveryPending_ = false;
  bool bootRecoveryReady_ = false;
  bool bootRecoveryFailed_ = false;
  bool recoveryFinalizeActive_ = false;
  RecorderStartState startState_;
  bool synchronousStartSucceeded_ = false;
  bool recoveryCandidate_ = false;
  bool recoveryAudioCommitted_ = false;
  RecoveryPhase recoveryPhase_ = RecoveryPhase::transaction;
  String recoveryDirectory_;
  String recoveryId_;
  String recoveryCheckpointPath_;
  StoredRecorderCheckpoint recoveryCheckpoint_{};
  uint32_t recoveryDataBytes_ = 0;
  uint32_t recoveryActualBytes_ = 0;
  uint32_t recoveryRemaining_ = 0;
  uint32_t recoveryCrcState_ = recorderAudioCrc32Begin();
  StorageAccess recoveryFileAccess_ = StorageAccess::read;
  uint16_t recoveryQuarantineSuffix_ = 0;
  uint8_t recoveryBuffer_[512]{};

#if defined(ARDUINO_ARCH_ESP32)
  class StorageCancellationGate final : public CapsuleTransactionGate {
   public:
    void reset() { cancelled_.store(false, std::memory_order_release); }
    void cancel() { cancelled_.store(true, std::memory_order_release); }
    bool permits(uint32_t) override {
      return !cancelled_.load(std::memory_order_acquire);
    }
   private:
    std::atomic<bool> cancelled_{false};
  };

  RecorderStorageQueue storageQueue_;
  RecorderStorageFrame *storageQueueSlots_ = nullptr;
  RecorderStorageFrame storageFrame_{};
  TaskHandle_t storageTask_ = nullptr;
  SemaphoreHandle_t storageStartAck_ = nullptr;
  Print *storageLog_ = nullptr;
  std::atomic<bool> storageSessionActive_{false};
  std::atomic<bool> storageStartRequested_{false};
  std::atomic<bool> storageStartSucceeded_{false};
  std::atomic<bool> storageStartCancelled_{false};
  std::atomic<bool> storageAbortRequested_{false};
  std::atomic<bool> storageGateObserved_{false};
  std::atomic<uint32_t> storageNowMs_{0};
  std::atomic<uint32_t> acceptedDataBytes_{0};
  StorageCancellationGate storageCancellationGate_;
  PeriodicCheckpointPhase periodicCheckpointPhase_ =
      PeriodicCheckpointPhase::idle;
  StoredRecorderCheckpoint periodicCheckpoint_{};
  CheckpointByteSource periodicCheckpointSource_;
#endif
};

}  // namespace pokepod
