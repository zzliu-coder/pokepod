#include "WavRecorder.h"

#include "CapsulePolicy.h"
#include "RuntimeDiagnostics.h"
#include "WavFormat.h"

#include <unistd.h>
#if defined(ARDUINO_ARCH_ESP32)
#include <esp_heap_caps.h>
#endif

namespace pokepod {
namespace {

String processingJson(const String &id, uint32_t durationMs, const char *status,
                      uint32_t revision, const char *errorStage,
                      const char *error) {
  String value;
  value.reserve(640);
  value += "{\n  \"schemaVersion\": 2,\n  \"capsuleId\": \"";
  value += id;
  value += "\",\n  \"revision\": ";
  value += revision;
  value += ",\n  \"durationMs\": ";
  value += durationMs;
  value += ",\n  \"status\": \"";
  value += status;
  value += "\",\n  \"audioFile\": \"";
  value += kCapsuleWavFile;
  value += "\",\n  \"audioFormat\": \"";
  value += kCapsuleWavFormat;
  value += "\",\n";
  value += "  \"sampleRateHz\": 16000,\n  \"channels\": 1,\n";
  value += "  \"bitsPerSample\": 16,\n  \"rawTextFile\": null,\n";
  value += "  \"polishedTextFile\": null,\n  \"errorStage\": ";
  if (errorStage == nullptr) value += "null";
  else {
    value += '"';
    value += errorStage;
    value += '"';
  }
  value += ",\n  \"error\": ";
  if (error == nullptr) value += "null";
  else {
    value += '"';
    value += error;
    value += '"';
  }
  value += ",\n  \"attempts\": 0,\n  \"engine\": \"tencent-asr\",\n";
  value += "  \"model\": \"16k_zh\"\n}\n";
  return value;
}

String capsuleJson(const String &id, const String &timestamp) {
  String value;
  value.reserve(512);
  value += "{\n  \"schemaVersion\": 1,\n  \"id\": \"";
  value += id;
  value += "\",\n  \"title\": \"语音胶囊\",\n  \"createdAt\": \"";
  value += timestamp;
  value += "\",\n  \"updatedAt\": \"";
  value += timestamp;
  value += "\",\n  \"revision\": 1,\n  \"favorite\": false,\n";
  value += "  \"tags\": [],\n  \"language\": \"zh\",\n  \"contentHash\": null\n}\n";
  return value;
}

}  // namespace

bool WavRecorder::begin(fs::FS &fs,
                        RecordingCapacitySource &capacitySource,
                        Print &log) {
  fs_ = &fs;
  capacitySource_ = &capacitySource;
#if !defined(ARDUINO_ARCH_ESP32)
  recordingProbeBuffer_ = recordingProbeStorage_;
#endif
#if defined(ARDUINO_ARCH_ESP32)
  if (!ensureStorageTask(log)) return false;
#endif
  if (!transaction_.begin(fs, log) ||
      !transactionRunner_.begin(fs, log) ||
      !transactionRunner_.startRecovery(StorageOwner::recovery)) return false;
  bootRecoveryPending_ = true;
  bootRecoveryReady_ = false;
  bootRecoveryFailed_ = false;
  recoveryPhase_ = RecoveryPhase::transaction;
  StorageReservation reservation = StorageCoordinator::instance().reserve(
      StorageOwner::recovery, StorageAccess::mutation, 1000);
  return reservation && ensureDirectory(kCapsuleRoot, log) &&
         ensureDirectory(kCapsuleInbox, log) &&
         ensureDirectory(kCapsuleArchive, log) &&
         ensureDirectory(kCapsuleStaging, log) &&
         ensureDirectory(kCapsuleTrash, log) &&
         ensureDirectory(kCapsuleSystem, log);
}

void WavRecorder::recordRuntime(RuntimeDiagnosticStage stage,
                                RuntimeDiagnosticOutcome outcome,
                                uint32_t detail0, uint32_t detail1,
                                Print &log) {
#if !defined(ARDUINO)
  (void)stage;
  (void)outcome;
  (void)detail0;
  (void)detail1;
  (void)log;
  return;
#else
  if (runtimeDiagnostics_ == nullptr) return;
  (void)runtimeDiagnostics_->record(RuntimeDiagnosticSubsystem::recording,
                                    stage, outcome, detail0, detail1, log);
#endif
}

bool WavRecorder::start(Print &log, const String &recordingId,
                        const String &createdAt) {
  return startInternal(log, recordingId, createdAt,
                       RecorderOperationOwner::localApp);
}

bool WavRecorder::start(Print &log, const String &recordingId,
                        const String &createdAt,
                        RecorderOperationOwner owner) {
  return startInternal(log, recordingId, createdAt, owner);
}

bool WavRecorder::startInternal(Print &log, const String &recordingId,
                                const String &createdAt,
                                RecorderOperationOwner owner) {
  if (!requestStartInternal(log, recordingId, createdAt, owner)) return false;
#if defined(ARDUINO_ARCH_ESP32)
  while (true) {
    const RecorderStartPollResult result = pollStart(log, millis(), nullptr);
    if (result == RecorderStartPollResult::pending) {
      delay(1);
      continue;
    }
    return result == RecorderStartPollResult::started;
  }
#else
  return pollStart(log, 0, nullptr) == RecorderStartPollResult::started;
#endif
}

bool WavRecorder::requestStart(Print &log, const String &recordingId,
                               const String &createdAt,
                               RecorderOperationOwner owner) {
  return requestStartInternal(log, recordingId, createdAt, owner);
}

bool WavRecorder::requestStartInternal(Print &log,
                                       const String &recordingId,
                                       const String &createdAt,
                                       RecorderOperationOwner owner) {
  recordRuntime(RuntimeDiagnosticStage::recordingRequest,
                RuntimeDiagnosticOutcome::started,
                static_cast<uint32_t>(owner), recordingId.length(), log);
  if (operationActive() || bootRecoveryFailed_ || file_ ||
      terminalState_.peek().pending()) {
    log.println("{\"event\":\"recording_error\",\"stage\":\"invalid_start\"}");
    recordRuntime(RuntimeDiagnosticStage::recordingFailure,
                  RuntimeDiagnosticOutcome::failure,
                  static_cast<uint32_t>(RecorderFailureStage::invalidStart),
                  static_cast<uint32_t>(RecorderTerminal::admissionFailure),
                  log);
    return false;
  }
  resetSessionState();
  operationOwner_ = owner;
  if (fs_ == nullptr || capacitySource_ == nullptr ||
      owner == RecorderOperationOwner::none ||
      !isUuid(recordingId.c_str()) || createdAt.isEmpty()) {
    return finishFailure(log, RecorderTerminal::admissionFailure,
                         RecorderFailureStage::invalidStart);
  }

  // Identity is accepted by the control plane now; all filesystem paths are
  // deliberately deferred until capacity has been read under storage
  // ownership. This keeps an admission failure from creating or cleaning a
  // staging directory that never belonged to the session.
  recordingId_ = recordingId;
  createdAt_ = createdAt;

#if defined(ARDUINO_ARCH_ESP32)
  storageLog_ = &log;
  storageQueue_.reset();
  acceptedDataBytes_.store(0, std::memory_order_release);
  storageGateObserved_.store(owner != RecorderOperationOwner::linkWifi,
                             std::memory_order_release);
  storageCancellationGate_.reset();
  periodicCheckpointPhase_ = PeriodicCheckpointPhase::idle;
  storageStartCancelled_.store(false, std::memory_order_release);
  storageAbortRequested_.store(false, std::memory_order_release);
  storageStartSucceeded_.store(false, std::memory_order_release);
  while (xSemaphoreTake(storageStartAck_, 0) == pdTRUE) {}
  const uint32_t startWaitBeganMs = millis();
  if (!startState_.begin(startWaitBeganMs)) return false;
  storageSessionActive_.store(true, std::memory_order_release);
  storageStartRequested_.store(true, std::memory_order_release);
  xTaskNotifyGive(storageTask_);
  return true;
#else
  if (!startState_.begin(0)) return false;
  synchronousStartSucceeded_ = startStorageSession(log);
  return true;
#endif
}

RecorderStartPollResult WavRecorder::pollStart(
    Print &log, uint32_t nowMs, CapsuleTransactionGate *gate) {
  if (!startState_.active()) return RecorderStartPollResult::idle;
#if defined(ARDUINO_ARCH_ESP32)
  const bool permitted = capsuleTransactionPermitted(gate, nowMs);
  const bool acknowledged =
      xSemaphoreTake(storageStartAck_, 0) == pdTRUE;
  const bool succeeded =
      storageStartSucceeded_.load(std::memory_order_acquire);
#else
  const bool permitted = capsuleTransactionPermitted(gate, nowMs);
  const bool acknowledged = true;
  const bool succeeded = synchronousStartSucceeded_;
#endif
  const RecorderStartPollResult result = startState_.poll(
      nowMs, permitted, acknowledged, succeeded);
  if (result == RecorderStartPollResult::cancelled) {
#if defined(ARDUINO_ARCH_ESP32)
    storageStartCancelled_.store(true, std::memory_order_release);
#endif
    // A denied/expired start acknowledgement is itself a terminal fact. This
    // also catches the race where the storage task completed staging just
    // before the main task observed the absolute Wi-Fi deadline.
    reportCaptureFailure(log, RecorderTerminal::cancelled,
                         RecorderFailureStage::storageBusy);
#if defined(ARDUINO_ARCH_ESP32)
    xTaskNotifyGive(storageTask_);
#endif
    log.println(
        "{\"event\":\"recording_error\",\"stage\":\"storage_start_timeout\"}");
  } else if (result == RecorderStartPollResult::started ||
             result == RecorderStartPollResult::failed) {
    log.printf(
        "{\"event\":\"recording_storage_start_ack\",\"ok\":%s}\n",
        result == RecorderStartPollResult::started ? "true" : "false");
  }
  return result;
}

bool WavRecorder::storageStartCancelled() const {
#if defined(ARDUINO_ARCH_ESP32)
  return storageStartCancelled_.load(std::memory_order_acquire);
#else
  return false;
#endif
}

uint32_t WavRecorder::storageReservationTimeoutMs() const {
  return operationOwner_.load(std::memory_order_acquire) ==
             RecorderOperationOwner::linkWifi
      ? 0U
      : 250U;
}

uint32_t WavRecorder::storageIoTimeoutMs() const {
  return operationOwner_.load(std::memory_order_acquire) ==
             RecorderOperationOwner::linkWifi
      ? 0U
      : 1000U;
}

void WavRecorder::requestStorageQualificationInvalidation(
    RecordingQualificationInvalidReason reason) {
  storageQualification_.requestInvalidation(reason);
}

void WavRecorder::consumeStorageQualificationInvalidations() {
  (void)storageQualification_.consumeInvalidationRequests();
}

void WavRecorder::observeStorageWriteLatency(uint32_t elapsedUs) {
  sessionTelemetry_.recordStorageWrite(elapsedUs);
}

void WavRecorder::updateRecorderTelemetry(bool forceStackSample) {
#if defined(ARDUINO_ARCH_ESP32)
  const uint32_t nowMs = millis();
  const bool sampleStack = forceStackSample ||
      !recorderTelemetryStackSampled_.load(std::memory_order_acquire) ||
      static_cast<uint32_t>(nowMs -
          recorderTelemetryLastStackSampleMs_.load(std::memory_order_acquire)) >=
          1000U;
  if (sampleStack && storageTask_ != nullptr) {
    recorderTelemetryStackHighWaterWords_.store(
        static_cast<uint32_t>(uxTaskGetStackHighWaterMark(storageTask_)),
        std::memory_order_release);
    recorderTelemetryLastStackSampleMs_.store(nowMs,
                                               std::memory_order_release);
    recorderTelemetryStackSampled_.store(true, std::memory_order_release);
  }
  sessionTelemetry_.observeRecorderQueue(
      static_cast<uint32_t>(storageQueue_.highWater()), storageQueue_.dropped(),
      recorderTelemetryStackHighWaterWords_.load(std::memory_order_acquire));
#else
  (void)forceStackSample;
  sessionTelemetry_.observeRecorderQueue(0, 0, 0);
#endif
}

void WavRecorder::freezeSessionTelemetry(Print &log) {
  sessionTelemetry_.freeze();
  const AudioSessionTelemetrySnapshot value = sessionTelemetry_.snapshot();
  #if defined(ARDUINO)
  if (runtimeDiagnostics_ != nullptr) {
    // This is an in-memory publication only.  RuntimeDiagnostics deliberately
    // does not persist the large capture snapshot on the recording path.
    runtimeDiagnostics_->publishAudioSessionSnapshot(value);
  }
  #endif
  log.printf(
      "{\"event\":\"recording_audio_telemetry\","
      "\"generation\":%lu,\"session_id\":%lu,"
      "\"read_calls\":%lu,\"short_reads\":%lu,"
      "\"partial_mono_samples\":%lu,"
      "\"capture_ring_high_water_frames\":%lu,"
      "\"capture_ring_dropped_frames\":%lu,"
      "\"recorder_queue_high_water_frames\":%lu,"
      "\"recorder_queue_dropped_frames\":%lu,"
      "\"dispatch_consumed_frames\":%lu,"
      "\"dispatch_routing_failures\":%lu,"
      "\"dispatch_recorder_delivery_failures\":%lu,"
      "\"dispatch_ble_delivery_failures\":%lu,"
      "\"dispatch_sequence_gaps\":%lu,"
      "\"dispatcher_max_us\":%lu,\"dispatcher_p99_us\":%lu,"
      "\"i2s_timeouts\":%lu,\"i2s_longest_read_us\":%lu,"
      "\"zero_byte_reads\":%lu,"
      "\"early_zero_reads\":%lu,\"source_overruns\":%lu,"
      "\"source_failures\":%lu,\"sequence_gaps\":%lu,"
      "\"storage_write_p99_us\":%lu,\"storage_write_p999_us\":%lu,"
      "\"capture_stack_high_water_words\":%lu,"
      "\"recorder_stack_high_water_words\":%lu,"
      "\"first_failure\":\"%s\",\"first_failure_at_ms\":%lu,"
      "\"first_failure_sequence\":%lu,"
      "\"source_overrun_observable\":%s,\"frozen\":true}\n",
      static_cast<unsigned long>(value.generation),
      static_cast<unsigned long>(value.sessionId),
      static_cast<unsigned long>(value.readCalls),
      static_cast<unsigned long>(value.shortReads),
      static_cast<unsigned long>(value.partialMonoSamples),
      static_cast<unsigned long>(value.captureRingHighWaterFrames),
      static_cast<unsigned long>(value.captureRingDroppedFrames),
      static_cast<unsigned long>(value.recorderQueueHighWaterFrames),
      static_cast<unsigned long>(value.recorderQueueDroppedFrames),
      static_cast<unsigned long>(value.dispatchConsumedFrames),
      static_cast<unsigned long>(value.dispatchRoutingFailures),
      static_cast<unsigned long>(value.dispatchRecorderDeliveryFailures),
      static_cast<unsigned long>(value.dispatchBleDeliveryFailures),
      static_cast<unsigned long>(value.dispatchSequenceGaps),
      static_cast<unsigned long>(value.dispatcherMaximumIntervalUs),
      static_cast<unsigned long>(value.dispatcherP99IntervalUs),
      static_cast<unsigned long>(value.i2sTimeouts),
      static_cast<unsigned long>(value.i2sLongestReadUs),
      static_cast<unsigned long>(value.zeroByteReads),
      static_cast<unsigned long>(value.earlyZeroReads),
      static_cast<unsigned long>(value.sourceOverruns),
      static_cast<unsigned long>(value.sourceFailures),
      static_cast<unsigned long>(value.sequenceGaps),
      static_cast<unsigned long>(value.storageWriteP99Us),
      static_cast<unsigned long>(value.storageWriteP999Us),
      static_cast<unsigned long>(value.captureTaskStackHighWaterWords),
      static_cast<unsigned long>(value.recorderTaskStackHighWaterWords),
      audioCaptureFailureCodeName(value.firstFailure),
      static_cast<unsigned long>(value.firstFailureAtMs),
      static_cast<unsigned long>(value.firstFailureSequence),
      value.sourceOverrunObservable ? "true" : "false");
}

bool WavRecorder::runStoragePerformanceProbe(Print &log) {
  consumeStorageQualificationInvalidations();
  activeQualificationProbeEpoch_ = storageQualification_.invalidationEpoch();
  if (capacitySource_ == nullptr || recordingProbeBuffer_ == nullptr) {
    return finishFailure(log, RecorderTerminal::admissionFailure,
                         RecorderFailureStage::capacityUnknown);
  }
  const String probePath = directory_ + "/.recording-probe.tmp";
  File probe;
  recordRuntime(RuntimeDiagnosticStage::recordingProbeOpen,
                RuntimeDiagnosticOutcome::started, 0, 0, log);
  {
    StorageIoLease lease = StorageCoordinator::instance().acquireIo(
        StorageOwner::recorder, StorageAccess::mutation,
        storageIoTimeoutMs());
    if (!lease) {
      return finishFailure(log, RecorderTerminal::storageFailure,
                           RecorderFailureStage::storageBusy);
    }
    probe = fs_->open(probePath, FILE_WRITE);
  }
  if (!probe) {
    recordRuntime(RuntimeDiagnosticStage::recordingProbeOpen,
                  RuntimeDiagnosticOutcome::failure,
                  static_cast<uint32_t>(RecorderFailureStage::storageProbeOpen),
                  0, log);
    return finishFailure(log, RecorderTerminal::storageFailure,
                         RecorderFailureStage::storageProbeOpen);
  }
  recordRuntime(RuntimeDiagnosticStage::recordingProbeOpen,
                RuntimeDiagnosticOutcome::success, 0, 0, log);

  RecorderTerminal failureTerminal = RecorderTerminal::none;
  RecorderFailureStage failureStage = RecorderFailureStage::none;
  uint64_t maximumTailUs = 0;
  uint32_t completedBytes = 0;
  uint8_t failedChunk = kRecordingProbeChunkCount;
  const uint64_t probeStartedUs = capacitySource_->monotonicMicros();
  uint64_t probeFinishedUs = probeStartedUs;

  for (uint8_t index = 0; index < kRecordingProbeChunkCount; ++index) {
    consumeStorageQualificationInvalidations();
    if (storageStartCancelled()) {
      failureTerminal = RecorderTerminal::cancelled;
      failureStage = RecorderFailureStage::storageBusy;
      break;
    }
    StorageIoLease lease = StorageCoordinator::instance().acquireIo(
        StorageOwner::recorder, StorageAccess::mutation,
        storageIoTimeoutMs());
    if (!lease) {
      failureTerminal = RecorderTerminal::storageFailure;
      failureStage = RecorderFailureStage::storageBusy;
      break;
    }
    const uint64_t primitiveStartedUs = capacitySource_->monotonicMicros();
    const size_t written = probe.write(recordingProbeBuffer_,
                                       kRecordingProbeChunkBytes);
    probe.flush();
    const uint64_t primitiveFinishedUs = capacitySource_->monotonicMicros();
    probeFinishedUs = primitiveFinishedUs;
    const uint64_t tailUs = primitiveFinishedUs - primitiveStartedUs;
    if (tailUs > maximumTailUs) maximumTailUs = tailUs;
    if (written != kRecordingProbeChunkBytes || probe.getWriteError() != 0) {
      failedChunk = index;
      failureTerminal = RecorderTerminal::storageFailure;
      failureStage = RecorderFailureStage::storageProbeWrite;
      break;
    }
    completedBytes += static_cast<uint32_t>(written);
    if (tailUs > kRecordingProbeMaximumTailUs) {
      failedChunk = index;
      failureTerminal = RecorderTerminal::admissionFailure;
      failureStage = RecorderFailureStage::storageTooSlow;
      break;
    }
  }

  const uint64_t totalUs = probeFinishedUs - probeStartedUs;
  if (failureStage == RecorderFailureStage::none &&
      totalUs > kRecordingProbeMaximumTotalUs) {
    failureTerminal = RecorderTerminal::admissionFailure;
    failureStage = RecorderFailureStage::storageTooSlow;
  }

  bool cleanupOk = false;
  recordRuntime(RuntimeDiagnosticStage::recordingProbeClose,
                RuntimeDiagnosticOutcome::started, 0, 0, log);
  {
    StorageIoLease lease = StorageCoordinator::instance().acquireIo(
        StorageOwner::recorder, StorageAccess::mutation,
        storageIoTimeoutMs());
    if (lease) {
      const int errorBefore = probe.getWriteError();
      probe.close();
      const int errorAfter = probe.getWriteError();
      const bool removed = fs_->remove(probePath);
      cleanupOk = errorBefore == 0 && errorAfter == 0 && removed;
    }
  }
  recordRuntime(RuntimeDiagnosticStage::recordingProbeClose,
                cleanupOk ? RuntimeDiagnosticOutcome::success
                          : RuntimeDiagnosticOutcome::failure,
                cleanupOk ? 0U : 1U, 0, log);
  recordRuntime(RuntimeDiagnosticStage::recordingProbeRemove,
                cleanupOk ? RuntimeDiagnosticOutcome::success
                          : RuntimeDiagnosticOutcome::failure,
                cleanupOk ? 0U : 1U, 0, log);
  if (!cleanupOk && failureStage == RecorderFailureStage::none) {
    failureTerminal = RecorderTerminal::storageFailure;
    failureStage = RecorderFailureStage::storageProbeCleanup;
  }

  // RuntimeDiagnostics persists its ring to NVS.  Persisting once per 4 KiB
  // primitive used to put synchronous internal-flash writes inside the timed
  // SD probe, so the UI could report a fast card as slow and every admission
  // caused dozens of needless NVS writes.  Only aggregate facts are recorded,
  // after all latency measurements and file cleanup have finished.
  recordRuntime(RuntimeDiagnosticStage::recordingProbeWrite,
                failureStage == RecorderFailureStage::none
                    ? RuntimeDiagnosticOutcome::success
                    : RuntimeDiagnosticOutcome::failure,
                completedBytes, failedChunk, log);
  recordRuntime(RuntimeDiagnosticStage::recordingProbeFlush,
                failureStage == RecorderFailureStage::none
                    ? RuntimeDiagnosticOutcome::success
                    : RuntimeDiagnosticOutcome::failure,
                static_cast<uint32_t>(totalUs / 1000ULL),
                static_cast<uint32_t>(maximumTailUs / 1000ULL), log);

  log.printf(
      "{\"event\":\"recording_storage_probe\",\"ok\":%s,"
      "\"bytes\":%lu,\"total_us\":%llu,\"maximum_tail_us\":%llu}\n",
      failureStage == RecorderFailureStage::none ? "true" : "false",
      static_cast<unsigned long>(kRecordingProbeBytes),
      static_cast<unsigned long long>(totalUs),
      static_cast<unsigned long long>(maximumTailUs));

  if (failureStage != RecorderFailureStage::none) {
    storageQualification_.recordProbeFailure(activeMountGeneration_,
                                              activeQualificationProbeEpoch_);
    return finishFailure(log, failureTerminal, failureStage);
  }
  if (!storageQualification_.recordSuccess(
          activeMountGeneration_, probeFinishedUs, totalUs, maximumTailUs,
          activeQualificationProbeEpoch_)) {
    return finishFailure(log, RecorderTerminal::admissionFailure,
                         RecorderFailureStage::storageTooSlow);
  }
  return true;
}

bool WavRecorder::startStorageSession(Print &log) {
  consumeStorageQualificationInvalidations();
  recordRuntime(RuntimeDiagnosticStage::recordingStorageReserve,
                RuntimeDiagnosticOutcome::started, 0, 0, log);
  storageReservation_ = StorageCoordinator::instance().reserve(
      StorageOwner::recorder, StorageAccess::mutation,
      storageReservationTimeoutMs());
  if (!storageReservation_) {
    recordRuntime(RuntimeDiagnosticStage::recordingStorageReserve,
                  RuntimeDiagnosticOutcome::failure,
                  static_cast<uint32_t>(RecorderFailureStage::storageBusy), 0,
                  log);
    return finishFailure(log, RecorderTerminal::storageFailure,
                         RecorderFailureStage::storageBusy);
  }
  recordRuntime(RuntimeDiagnosticStage::recordingStorageReserve,
                RuntimeDiagnosticOutcome::success,
                static_cast<uint32_t>(storageReservation_.owner()), 0, log);
  if (storageStartCancelled()) {
    return finishFailure(log, RecorderTerminal::cancelled,
                         RecorderFailureStage::storageBusy);
  }

  // Capacity is read exactly once, inside both ownership layers, immediately
  // before staging creation. App and Link never obtain a stale snapshot.
  RecordingSpaceSnapshot space{};
  recordRuntime(RuntimeDiagnosticStage::recordingCapacity,
                RuntimeDiagnosticOutcome::started, 0, 0, log);
  {
    StorageIoLease capacityIo = StorageCoordinator::instance().acquireIo(
        StorageOwner::recorder, StorageAccess::mutation,
        storageIoTimeoutMs());
    if (!capacityIo) {
      recordRuntime(RuntimeDiagnosticStage::recordingCapacity,
                    RuntimeDiagnosticOutcome::failure,
                    static_cast<uint32_t>(RecorderFailureStage::storageBusy), 0,
                    log);
      return finishFailure(log, RecorderTerminal::storageFailure,
                           RecorderFailureStage::storageBusy);
    }
    space = capacitySource_->query();
  }
  recordRuntime(RuntimeDiagnosticStage::recordingCapacity,
                RuntimeDiagnosticOutcome::success,
                static_cast<uint32_t>((space.totalBytes - space.usedBytes) /
                                      1024ULL),
                static_cast<uint32_t>(space.totalBytes / 1024ULL), log);
  const RecordingAdmission admission = evaluateRecordingAdmission(space);
  if (!admission.allowed()) {
    RecorderFailureStage stage = RecorderFailureStage::insufficientSpace;
    if (admission.reason == RecordingAdmissionReason::capacityUnknown) {
      stage = RecorderFailureStage::capacityUnknown;
      storageQualification_.invalidateOwned(
          RecordingQualificationInvalidReason::capacityUnknown);
    } else if (admission.reason == RecordingAdmissionReason::invalidCapacity) {
      stage = RecorderFailureStage::capacityInvalid;
      storageQualification_.invalidateOwned(
          RecordingQualificationInvalidReason::capacityInvalid);
    } else {
      storageQualification_.invalidateOwned(
          RecordingQualificationInvalidReason::lowSpace);
    }
    log.printf(
        "{\"event\":\"recording_admission_rejected\",\"stage\":\"%s\","
        "\"available_bytes\":%llu,\"required_bytes\":%llu}\n",
        recorderFailureStageName(stage),
        static_cast<unsigned long long>(admission.availableBytes),
        static_cast<unsigned long long>(admission.requiredBytes));
    return finishFailure(log, RecorderTerminal::admissionFailure, stage);
  }
  activeMountGeneration_ = capacitySource_->mountGeneration();
  const uint64_t qualificationNowUs = capacitySource_->monotonicMicros();
  const RecordingQualificationDecision qualification =
      storageQualification_.decision(activeMountGeneration_,
                                     qualificationNowUs);
  if (qualification == RecordingQualificationDecision::rejectUnavailable) {
    return finishFailure(log, RecorderTerminal::admissionFailure,
                         RecorderFailureStage::capacityUnknown);
  }
  if (storageStartCancelled()) {
    return finishFailure(log, RecorderTerminal::cancelled,
                         RecorderFailureStage::storageBusy);
  }

  directory_ = String(kCapsuleStaging) + "/" + recordingId_;
  partialPath_ = directory_ + "/audio.wav.part";
  finalPath_ = directory_ + "/audio.wav";
  {
    StorageIoLease startIo = StorageCoordinator::instance().acquireIo(
        StorageOwner::recorder, StorageAccess::mutation,
        storageIoTimeoutMs());
    if (!startIo) {
      return finishFailure(log, RecorderTerminal::storageFailure,
                           RecorderFailureStage::storageBusy);
    }
    if (fs_->exists(directory_)) {
      return finishFailure(log, RecorderTerminal::commitFailure,
                           RecorderFailureStage::stagingExists);
    }
    if (!fs_->mkdir(directory_)) {
      return finishFailure(log, RecorderTerminal::storageFailure,
                           RecorderFailureStage::createDirectory);
    }
  }
  if (storageStartCancelled()) {
    return finishFailure(log, RecorderTerminal::cancelled,
                         RecorderFailureStage::storageBusy);
  }
  if (qualification == RecordingQualificationDecision::probe &&
      !runStoragePerformanceProbe(log)) return false;
  if (qualification == RecordingQualificationDecision::reuse) {
    log.printf(
        "{\"event\":\"recording_storage_probe\",\"ok\":true,"
        "\"cached\":true,\"mount_generation\":%lu}\n",
        static_cast<unsigned long>(activeMountGeneration_));
  }
  if (storageStartCancelled()) {
    return finishFailure(log, RecorderTerminal::cancelled,
                         RecorderFailureStage::storageBusy);
  }

  checkpointInitialized_ = initializeRecorderCheckpoint(
      checkpoint_, recordingId_.c_str(), createdAt_.c_str());
  if (!checkpointInitialized_) {
    return finishFailure(log, RecorderTerminal::metadataFailure,
                         RecorderFailureStage::recoveryCheckpoint);
  }
  if (!writeProcessingMetadata(log, "recording", 1, nullptr, nullptr)) {
    recordRuntime(RuntimeDiagnosticStage::recordingMetadata,
                  RuntimeDiagnosticOutcome::failure,
                  static_cast<uint32_t>(RecorderFailureStage::initialMetadata),
                  0, log);
    return finishFailure(log, RecorderTerminal::metadataFailure,
                         RecorderFailureStage::initialMetadata);
  }
  recordRuntime(RuntimeDiagnosticStage::recordingMetadata,
                RuntimeDiagnosticOutcome::success, 0, 0, log);
  if (storageStartCancelled()) {
    return finishFailure(log, RecorderTerminal::cancelled,
                         RecorderFailureStage::initialMetadata);
  }

  StorageIoLease openIo = StorageCoordinator::instance().acquireIo(
      StorageOwner::recorder, StorageAccess::mutation, storageIoTimeoutMs());
  if (!openIo) {
    return finishFailure(log, RecorderTerminal::storageFailure,
                         RecorderFailureStage::storageBusy);
  }
  file_ = fs_->open(partialPath_, FILE_WRITE);
  if (!file_) {
    recordRuntime(RuntimeDiagnosticStage::recordingAudioOpen,
                  RuntimeDiagnosticOutcome::failure,
                  static_cast<uint32_t>(RecorderFailureStage::openAudio), 0,
                  log);
    return finishFailure(log, RecorderTerminal::storageFailure,
                         RecorderFailureStage::openAudio);
  }
  recordRuntime(RuntimeDiagnosticStage::recordingAudioOpen,
                RuntimeDiagnosticOutcome::success, 0, 0, log);
  if (!writeHeader(0)) {
    return finishFailure(log, RecorderTerminal::headerFailure,
                         RecorderFailureStage::initialHeader);
  }
  openIo.release();
  if (!persistCheckpoint(false, RecorderFailureStage::none, log)) {
    return finishFailure(log, RecorderTerminal::metadataFailure,
                         RecorderFailureStage::recoveryCheckpoint);
  }
  if (storageStartCancelled()) {
    return finishFailure(log, RecorderTerminal::cancelled,
                         RecorderFailureStage::recoveryCheckpoint);
  }
  recording_ = true;
  recordRuntime(RuntimeDiagnosticStage::recordingStarted,
                RuntimeDiagnosticOutcome::success, dataBytes_,
                activeMountGeneration_, log);
  updateRecorderTelemetry(true);
  log.printf(
      "{\"event\":\"recording_started\",\"id\":\"%s\","
      "\"format\":\"16k_s16le_mono\"}\n",
      recordingId_.c_str());
  return true;
}

bool WavRecorder::appendMono16(const int16_t *samples, size_t sampleCount,
                               Print &log) {
  if (samples == nullptr || sampleCount == 0 ||
      sampleCount > SIZE_MAX / sizeof(int16_t)) {
    return false;
  }
  if (!appendMonoBytes(reinterpret_cast<const uint8_t *>(samples),
                       sampleCount * sizeof(int16_t), log)) {
    return false;
  }
  if (durationMs() >= kMaxRecordingMs) {
    automaticStopRequested_ = true;
    automaticStopReason_ = RecorderStopReason::maxDuration;
  }
  return true;
}

bool WavRecorder::appendMonoBytes(const uint8_t *data, size_t length,
                                  Print &log) {
  if (automaticStopRequested_) return true;
#if defined(ARDUINO_ARCH_ESP32)
  if (!recording_ || data == nullptr || length == 0 ||
      (length & 1U) != 0) {
    return false;
  }
#else
  if (!recording_ || !file_ || data == nullptr || length == 0 ||
      (length & 1U) != 0) {
    return false;
  }
#endif
#if defined(ARDUINO_ARCH_ESP32)
  const uint32_t accepted = acceptedDataBytes_.load(std::memory_order_acquire);
  if (accepted >= kMaximumRecordingAudioBytes) return false;
  const size_t remaining = static_cast<size_t>(
      kMaximumRecordingAudioBytes - accepted);
  if (length > remaining) length = remaining & ~static_cast<size_t>(1U);
  if (length == 0 || !storageQueue_.push(data, length)) {
    requestStorageQualificationInvalidation(
        RecordingQualificationInvalidReason::queueHighWater);
    updateRecorderTelemetry();
    reportCaptureFailure(log, RecorderTerminal::storageFailure,
                         RecorderFailureStage::storageQueueOverflow);
    return false;
  }
  acceptedDataBytes_.fetch_add(static_cast<uint32_t>(length),
                               std::memory_order_release);
  updateRecorderTelemetry();
  if (storageQueue_.highWater() >=
      kRecorderQualificationHighWaterFrames) {
    requestStorageQualificationInvalidation(
        RecordingQualificationInvalidReason::queueHighWater);
  }
  xTaskNotifyGive(storageTask_);
  return true;
#else
  return storageAppendMonoBytes(data, length, log);
#endif
}

bool WavRecorder::storageAppendMonoBytes(const uint8_t *data, size_t length,
                                         Print &log) {
  consumeStorageQualificationInvalidations();
  if (!file_ || data == nullptr || length == 0 || (length & 1U) != 0) {
    return false;
  }
  if (dataBytes_ >= kMaximumRecordingAudioBytes) return false;
  const size_t remaining = static_cast<size_t>(
      kMaximumRecordingAudioBytes - dataBytes_);
  if (length > remaining) length = remaining & ~static_cast<size_t>(1U);
  if (length == 0) return false;
  StorageIoLease writeIo = StorageCoordinator::instance().acquireIo(
      StorageOwner::recorder, StorageAccess::mutation, storageIoTimeoutMs());
  if (!writeIo) {
    return finishFailure(log, RecorderTerminal::storageFailure,
                         RecorderFailureStage::storageBusy);
  }
  const uint64_t writeStartedUs = capacitySource_->monotonicMicros();
  const size_t written = file_.write(data, length);
  const uint64_t writeFinishedUs = capacitySource_->monotonicMicros();
  observeStorageWriteLatency(static_cast<uint32_t>(
      writeFinishedUs - writeStartedUs));
  dataBytes_ += static_cast<uint32_t>(written);
  checkpointCrcState_ = recorderAudioCrc32Update(
      checkpointCrcState_, data, written);
  if (written != length) {
    storageQualification_.invalidateOwned(
        RecordingQualificationInvalidReason::shortWrite);
    log.printf("{\"event\":\"recording_error\",\"stage\":\"short_write\",\"expected\":%u,\"actual\":%u}\n",
               static_cast<unsigned>(length), static_cast<unsigned>(written));
    return finishFailure(log, RecorderTerminal::storageFailure,
                         RecorderFailureStage::shortWrite);
  }
  writeIo.release();
#if defined(ARDUINO_ARCH_ESP32)
  if (periodicCheckpointPhase_ == PeriodicCheckpointPhase::idle &&
      recorderCheckpointDue(checkpointedBytes_, dataBytes_)) {
    periodicCheckpoint_ = checkpoint_;
    updateRecorderCheckpoint(periodicCheckpoint_, dataBytes_,
                             recorderAudioCrc32Finish(checkpointCrcState_));
    periodicCheckpointSource_.bind(periodicCheckpoint_);
    periodicCheckpointPhase_ = PeriodicCheckpointPhase::flushAudio;
  }
#else
  if (recorderCheckpointDue(checkpointedBytes_, dataBytes_) &&
      !persistCheckpoint(false, RecorderFailureStage::none, log)) {
    return finishFailure(log, RecorderTerminal::metadataFailure,
                         RecorderFailureStage::recoveryCheckpoint);
  }
#endif
  return true;
}

bool WavRecorder::stop(Print &log, RecorderStopReason reason) {
  if (!recording_) return false;
  if (captureFailureLatched()) {
#if defined(ARDUINO_ARCH_ESP32)
    storageAbortRequested_.store(true, std::memory_order_release);
    xTaskNotifyGive(storageTask_);
#else
    (void)finishFailure(log, captureFailureTerminal(),
                        captureFailureStage());
#endif
    return true;
  }
  recording_ = false;
  automaticStopRequested_ = false;
  automaticStopReason_ = RecorderStopReason::none;
  finalizeStopReason_ = reason;
  finalizePrimitiveFailures_ = 0;
  finalizePhase_ = FinalizePhase::flushAudio;
  finalizePending_ = true;
#if defined(ARDUINO_ARCH_ESP32)
  const uint32_t publishedBytes =
      acceptedDataBytes_.load(std::memory_order_acquire);
#else
  const uint32_t publishedBytes = dataBytes_;
#endif
  log.printf("{\"event\":\"recording_finalize_pending\",\"bytes\":%lu}\n",
             static_cast<unsigned long>(publishedBytes));
#if defined(ARDUINO_ARCH_ESP32)
  xTaskNotifyGive(storageTask_);
#endif
  return true;
}

uint32_t WavRecorder::durationMs() const {
#if defined(ARDUINO_ARCH_ESP32)
  if (recording_ || storageSessionActive_.load(std::memory_order_acquire)) {
    return audioDurationMs(
        acceptedDataBytes_.load(std::memory_order_acquire));
  }
#endif
  return audioDurationMs(dataBytes_);
}

bool WavRecorder::abortCapture(Print &log) {
  if (!recording_ && !captureFailureLatched()) return false;
  reportCaptureFailure(log, RecorderTerminal::captureFailure,
                       RecorderFailureStage::captureIncomplete);
  return true;
}

void WavRecorder::reportCaptureFailure(Print &log,
                                       RecorderTerminal terminal,
                                       RecorderFailureStage stage) {
  if (terminal == RecorderTerminal::none ||
      terminal == RecorderTerminal::completed ||
      stage == RecorderFailureStage::none) {
    return;
  }
  const uint16_t code =
      (static_cast<uint16_t>(terminal) << 8U) |
      static_cast<uint16_t>(stage);
  uint16_t expected = 0;
  const bool first = captureFailureCode_.compare_exchange_strong(
      expected, code, std::memory_order_acq_rel, std::memory_order_acquire);
  if (first) {
    log.printf(
        "{\"event\":\"recording_error\",\"stage\":\"%s\",\"latched\":true}\n",
        recorderFailureStageName(stage));
  }
#if defined(ARDUINO_ARCH_ESP32)
  storageAbortRequested_.store(true, std::memory_order_release);
  if (storageTask_ != nullptr) xTaskNotifyGive(storageTask_);
#else
  const RecorderTerminal latchedTerminal = captureFailureTerminal();
  const RecorderFailureStage latchedStage = captureFailureStage();
  if (recording_ || finalizePending_) {
    (void)finishFailure(
        log, latchedTerminal == RecorderTerminal::none
                 ? RecorderTerminal::captureFailure
                 : latchedTerminal,
        latchedStage == RecorderFailureStage::none
                 ? RecorderFailureStage::captureIncomplete
                 : latchedStage);
  }
#endif
}

bool WavRecorder::finalizeFailure(Print &log, RecorderTerminal terminal,
                                  RecorderFailureStage stage) {
  (void)finishFailure(log, terminal, stage);
  return false;
}

bool WavRecorder::pollFinalizeRunner(Print &log, uint32_t nowMs,
                                     CapsuleTransactionGate *gate) {
  const CapsuleTransactionPollResult result = transactionRunner_.poll(nowMs,
                                                                       gate);
  if (result == CapsuleTransactionPollResult::progress ||
      result == CapsuleTransactionPollResult::wouldBlock) {
    return false;
  }
  if (result != CapsuleTransactionPollResult::committed) {
    const RecorderFailureStage stage =
        finalizePhase_ == FinalizePhase::pollAudioAndProcessing
            ? RecorderFailureStage::commitAudio
            : RecorderFailureStage::capsuleMetadata;
    log.printf(
        "{\"event\":\"recording_finalize_transaction_failed\","
        "\"result\":%u,\"phase\":\"%s\"}\n",
        static_cast<unsigned>(result), transactionRunner_.phaseName());
    RecorderTerminal terminal = RecorderTerminal::commitFailure;
    if (result == CapsuleTransactionPollResult::cancelled) {
      terminal = RecorderTerminal::cancelled;
    } else if (result == CapsuleTransactionPollResult::cleanupBlocked ||
               result == CapsuleTransactionPollResult::recoveryBlocked) {
      terminal = RecorderTerminal::cleanupBlocked;
    }
    return finalizeFailure(log, terminal, stage);
  }
  finalizePrimitiveFailures_ = 0;
  finalizePhase_ =
      finalizePhase_ == FinalizePhase::pollAudioAndProcessing
          ? FinalizePhase::startCapsuleMetadata
          : FinalizePhase::checkInboxDirectory;
  return false;
}

void WavRecorder::completeFinalize(Print &log) {
  if (!recoveryFinalizeActive_ && captureFailureLatched()) {
    (void)finishFailure(
        log, captureFailureTerminal() == RecorderTerminal::none
                 ? RecorderTerminal::captureFailure
                 : captureFailureTerminal(),
        captureFailureStage());
    return;
  }
  if (recoveryFinalizeActive_) {
    log.printf(
        "{\"event\":\"recording_recovered\",\"id\":\"%s\",\"duration_ms\":%lu}\n",
        recordingId_.c_str(), static_cast<unsigned long>(durationMs()));
    finalizePending_ = false;
    finalizePhase_ = FinalizePhase::idle;
    recoveryFinalizeActive_ = false;
    recoveryPhase_ = RecoveryPhase::scanNext;
    recordingId_ = "";
    createdAt_ = "";
    directory_ = "";
    partialPath_ = "";
    finalPath_ = "";
    dataBytes_ = 0;
    return;
  }
  finalizePhase_ = FinalizePhase::idle;
  updateRecorderTelemetry(true);
  freezeSessionTelemetry(log);
  terminalState_.complete(finalizeStopReason_, dataBytes_);
  const AudioFrontEndMetrics &audio = audioMetrics_;
  log.printf("{\"event\":\"recording_stopped\",\"ok\":true,\"duration_ms\":%lu,\"bytes\":%lu,\"path\":\"%s\",\"audio_channel\":\"%s\",\"left_peak\":%u,\"right_peak\":%u,\"output_peak\":%u,\"noise_floor\":%u,\"suppressed_samples\":%lu,\"limited_samples\":%lu,\"maximum_gain_q12\":%lu}\n",
             static_cast<unsigned long>(durationMs()),
             static_cast<unsigned long>(dataBytes_), finalPath_.c_str(),
             audioInputChannelName(audio.selectedChannel), audio.leftPeak,
             audio.rightPeak, audio.outputPeak, audio.estimatedNoiseFloor,
             static_cast<unsigned long>(audio.suppressedSamples),
             static_cast<unsigned long>(audio.limitedSamples),
             static_cast<unsigned long>(audio.maximumGainQ12));
#if defined(ARDUINO_ARCH_ESP32)
  log.printf(
      "{\"event\":\"recording_storage_queue\",\"high_water_frames\":%u,\"dropped_frames\":%lu,\"capacity_frames\":%u,\"task_stack_high_water_words\":%u}\n",
      static_cast<unsigned>(storageQueue_.highWater()),
      static_cast<unsigned long>(storageQueue_.dropped()),
      static_cast<unsigned>(kRecorderStorageQueueFrames),
      static_cast<unsigned>(recorderTelemetryStackHighWaterWords_.load(
          std::memory_order_acquire)));
#endif
  finalizeStopReason_ = RecorderStopReason::none;
  storageReservation_.release();
  operationOwner_ = RecorderOperationOwner::none;
  // This release-store is the cross-task publication boundary. The UI may
  // consume terminalState_ only after operationActive() observes false.
  finalizePending_.store(false, std::memory_order_release);
}

bool WavRecorder::pollBootRecovery(Print &log, uint32_t nowMs) {
  const auto quarantine = [&]() {
    recoveryCandidate_ = false;
    recoveryQuarantineSuffix_ = 0;
    recoveryPhase_ = file_ ? RecoveryPhase::quarantineCloseFile
                           : RecoveryPhase::quarantineDestinationExists;
    return false;
  };
  const auto fail = [&]() {
    bootRecoveryReady_ = false;
    recoveryCandidate_ = false;
    recoveryPhase_ = file_ ? RecoveryPhase::failureCloseFile
        : (recoveryEntry_ ? RecoveryPhase::failureCloseEntry
                          : (recoveryRoot_ ? RecoveryPhase::failureCloseRoot
                                           : RecoveryPhase::failureDone));
    return false;
  };

  switch (recoveryPhase_) {
    case RecoveryPhase::transaction: {
      const CapsuleTransactionPollResult result =
          transactionRunner_.poll(nowMs, nullptr);
      if (result == CapsuleTransactionPollResult::progress ||
          result == CapsuleTransactionPollResult::wouldBlock) return false;
      if (result != CapsuleTransactionPollResult::recovered) return fail();
      if (transactionRunner_.recoveryQuarantined()) {
        log.println(
            "{\"event\":\"recording_boot_recovery_quarantined\"}");
      }
      recoveryPhase_ = RecoveryPhase::reserve;
      return false;
    }
    case RecoveryPhase::reserve: {
      StorageReservation reservation = StorageCoordinator::instance().reserve(
          StorageOwner::recovery, StorageAccess::mutation, 0);
      if (!reservation) return false;
      storageReservation_ = std::move(reservation);
      recoveryPhase_ = RecoveryPhase::scanOpen;
      return false;
    }
    case RecoveryPhase::scanOpen: {
      StorageIoLease lease = StorageCoordinator::instance().acquireIo(
          StorageOwner::recovery, StorageAccess::read, 0);
      if (!lease) return false;
      recoveryRoot_ = fs_->open(kCapsuleStaging, FILE_READ);
      if (!recoveryRoot_ || !recoveryRoot_.isDirectory()) return fail();
      recoveryPhase_ = RecoveryPhase::scanNext;
      return false;
    }
    case RecoveryPhase::scanNext: {
      StorageIoLease lease = StorageCoordinator::instance().acquireIo(
          StorageOwner::recovery, StorageAccess::read, 0);
      if (!lease) return false;
      recoveryEntry_ = recoveryRoot_.openNextFile();
      if (!recoveryEntry_) {
        recoveryCandidate_ = false;
        recoveryPhase_ = RecoveryPhase::closeRoot;
        return false;
      }
      const String fullName = recoveryEntry_.name();
      const int slash = fullName.lastIndexOf('/');
      recoveryId_ = slash >= 0 ? fullName.substring(slash + 1) : fullName;
      recoveryDirectory_ = String(kCapsuleStaging) + "/" + recoveryId_;
      recoveryCandidate_ = recoveryEntry_.isDirectory() &&
          isUuid(recoveryId_.c_str());
      recoveryPhase_ = RecoveryPhase::closeEntry;
      return false;
    }
    case RecoveryPhase::closeEntry: {
      StorageIoLease lease = StorageCoordinator::instance().acquireIo(
          StorageOwner::recovery, StorageAccess::read, 0);
      if (!lease) return false;
      recoveryEntry_.close();
      recoveryPhase_ = recoveryCandidate_ ? RecoveryPhase::checkFailureMarker
                                          : RecoveryPhase::scanNext;
      return false;
    }
    case RecoveryPhase::closeRoot: {
      StorageIoLease lease = StorageCoordinator::instance().acquireIo(
          StorageOwner::recovery, StorageAccess::read, 0);
      if (!lease) return false;
      recoveryRoot_.close();
      recoveryPhase_ = RecoveryPhase::done;
      return false;
    }
    case RecoveryPhase::checkFailureMarker:
    case RecoveryPhase::checkFailedCheckpoint:
    case RecoveryPhase::checkQuarantinedCheckpoint:
    case RecoveryPhase::checkCheckpoint: {
      const String path = recoveryDirectory_ +
          (recoveryPhase_ == RecoveryPhase::checkFailureMarker
               ? "/recording.failure"
               : (recoveryPhase_ == RecoveryPhase::checkFailedCheckpoint
                      ? "/recording.failed.chk"
                      : (recoveryPhase_ ==
                                 RecoveryPhase::checkQuarantinedCheckpoint
                             ? "/recording.failure.chk"
                             : "/recording.chk")));
      StorageIoLease lease = StorageCoordinator::instance().acquireIo(
          StorageOwner::recovery, StorageAccess::read, 0);
      if (!lease) return false;
      const bool exists = fs_->exists(path);
      if ((recoveryPhase_ == RecoveryPhase::checkFailureMarker ||
           recoveryPhase_ == RecoveryPhase::checkFailedCheckpoint ||
           recoveryPhase_ == RecoveryPhase::checkQuarantinedCheckpoint) &&
          exists) {
        log.printf(
            "{\"event\":\"recording_recovery_preserved\",\"id\":\"%s\"}\n",
            recoveryId_.c_str());
        recoveryCandidate_ = false;
        recoveryPhase_ = RecoveryPhase::scanNext;
      } else if (recoveryPhase_ == RecoveryPhase::checkFailureMarker) {
        recoveryPhase_ = RecoveryPhase::checkFailedCheckpoint;
      } else if (recoveryPhase_ == RecoveryPhase::checkFailedCheckpoint) {
        recoveryPhase_ = RecoveryPhase::checkQuarantinedCheckpoint;
      } else if (recoveryPhase_ ==
                 RecoveryPhase::checkQuarantinedCheckpoint) {
        recoveryPhase_ = RecoveryPhase::checkCheckpoint;
      } else if (!exists) {
        recoveryPhase_ = RecoveryPhase::checkOrphanPartial;
      } else {
        recoveryCheckpointPath_ = path;
        recoveryPhase_ = RecoveryPhase::openCheckpoint;
      }
      return false;
    }
    case RecoveryPhase::checkOrphanPartial: {
      StorageIoLease lease = StorageCoordinator::instance().acquireIo(
          StorageOwner::recovery, StorageAccess::read, 0);
      if (!lease) return false;
      if (fs_->exists(recoveryDirectory_ + "/audio.wav.part")) {
        return quarantine();
      }
      recoveryCandidate_ = false;
      recoveryPhase_ = RecoveryPhase::scanNext;
      return false;
    }
    case RecoveryPhase::openCheckpoint: {
      StorageIoLease lease = StorageCoordinator::instance().acquireIo(
          StorageOwner::recovery, StorageAccess::read, 0);
      if (!lease) return false;
      file_ = fs_->open(recoveryCheckpointPath_, FILE_READ);
      recoveryFileAccess_ = StorageAccess::read;
      if (!file_) {
        if (++finalizePrimitiveFailures_ < 3) return false;
        finalizePrimitiveFailures_ = 0;
        return quarantine();
      }
      finalizePrimitiveFailures_ = 0;
      if (file_.isDirectory() ||
          file_.size() != sizeof(recoveryCheckpoint_)) return quarantine();
      recoveryPhase_ = RecoveryPhase::readCheckpoint;
      return false;
    }
    case RecoveryPhase::readCheckpoint: {
      StorageIoLease lease = StorageCoordinator::instance().acquireIo(
          StorageOwner::recovery, StorageAccess::read, 0);
      if (!lease) return false;
      const int bytes = file_.read(
          reinterpret_cast<uint8_t *>(&recoveryCheckpoint_),
          sizeof(recoveryCheckpoint_));
      if (bytes != static_cast<int>(sizeof(recoveryCheckpoint_)) ||
          !validateRecorderCheckpoint(recoveryCheckpoint_) ||
          recoveryCheckpoint_.state !=
              static_cast<uint8_t>(RecorderCheckpointState::recording) ||
          strcmp(recoveryCheckpoint_.capsuleId, recoveryId_.c_str()) != 0) {
        recoveryPhase_ = RecoveryPhase::closeCheckpoint;
        recoveryCandidate_ = false;
      } else {
        recoveryPhase_ = RecoveryPhase::closeCheckpoint;
      }
      return false;
    }
    case RecoveryPhase::closeCheckpoint: {
      StorageIoLease lease = StorageCoordinator::instance().acquireIo(
          StorageOwner::recovery, StorageAccess::read, 0);
      if (!lease) return false;
      file_.close();
      recoveryPhase_ = recoveryCandidate_ ? RecoveryPhase::checkCommittedAudio
                                          : RecoveryPhase::quarantineDestinationExists;
      return false;
    }
    case RecoveryPhase::checkCommittedAudio: {
      StorageIoLease lease = StorageCoordinator::instance().acquireIo(
          StorageOwner::recovery, StorageAccess::read, 0);
      if (!lease) return false;
      recoveryAudioCommitted_ =
          fs_->exists(recoveryDirectory_ + "/audio.wav");
      recoveryPhase_ = RecoveryPhase::openPartial;
      return false;
    }
    case RecoveryPhase::openPartial: {
      partialPath_ = recoveryDirectory_ +
          (recoveryAudioCommitted_ ? "/audio.wav" : "/audio.wav.part");
      StorageIoLease lease = StorageCoordinator::instance().acquireIo(
          StorageOwner::recovery, StorageAccess::read, 0);
      if (!lease) return false;
      file_ = fs_->open(partialPath_, FILE_READ);
      recoveryFileAccess_ = StorageAccess::read;
      if (!file_ || file_.isDirectory() || file_.size() <= kWavHeaderBytes ||
          file_.size() - kWavHeaderBytes > kMaximumRecordingAudioBytes) {
        return quarantine();
      }
      recoveryActualBytes_ =
          static_cast<uint32_t>(file_.size() - kWavHeaderBytes) & ~1U;
      recoveryDataBytes_ = recoveryCheckpoint_.confirmedDataBytes;
      if (recoveryDataBytes_ == 0 ||
          recoveryDataBytes_ > recoveryActualBytes_) return quarantine();
      recoveryRemaining_ = recoveryDataBytes_;
      recoveryCrcState_ = recorderAudioCrc32Begin();
      recoveryPhase_ = recoveryAudioCommitted_
          ? RecoveryPhase::readCommittedHeader : RecoveryPhase::seekPartial;
      return false;
    }
    case RecoveryPhase::readCommittedHeader: {
      uint8_t header[kWavHeaderBytes];
      StorageIoLease lease = StorageCoordinator::instance().acquireIo(
          StorageOwner::recovery, StorageAccess::read, 0);
      if (!lease) return false;
      const int bytes = file_.read(header, sizeof(header));
      uint32_t headerDataBytes = 0;
      if (bytes != static_cast<int>(sizeof(header)) ||
          !validCapsuleWavHeader(
              header, sizeof(header),
              kWavHeaderBytes + recoveryActualBytes_, headerDataBytes) ||
          headerDataBytes != recoveryDataBytes_ ||
          recoveryActualBytes_ != recoveryDataBytes_) {
        return quarantine();
      }
      recoveryPhase_ = RecoveryPhase::seekPartial;
      return false;
    }
    case RecoveryPhase::seekPartial: {
      StorageIoLease lease = StorageCoordinator::instance().acquireIo(
          StorageOwner::recovery, StorageAccess::read, 0);
      if (!lease) return false;
      if (!file_.seek(kWavHeaderBytes)) return quarantine();
      recoveryPhase_ = RecoveryPhase::readPartialCrc;
      return false;
    }
    case RecoveryPhase::readPartialCrc: {
      if (recoveryRemaining_ == 0) {
        recoveryCandidate_ =
            recorderAudioCrc32Finish(recoveryCrcState_) ==
            recoveryCheckpoint_.audioCrc32;
        recoveryPhase_ = RecoveryPhase::closePartial;
        return false;
      }
      const size_t wanted = recoveryRemaining_ < sizeof(recoveryBuffer_)
          ? recoveryRemaining_ : sizeof(recoveryBuffer_);
      StorageIoLease lease = StorageCoordinator::instance().acquireIo(
          StorageOwner::recovery, StorageAccess::read, 0);
      if (!lease) return false;
      const int bytes = file_.read(recoveryBuffer_, wanted);
      if (bytes != static_cast<int>(wanted)) return quarantine();
      recoveryCrcState_ = recorderAudioCrc32Update(
          recoveryCrcState_, recoveryBuffer_, wanted);
      recoveryRemaining_ -= static_cast<uint32_t>(wanted);
      return false;
    }
    case RecoveryPhase::closePartial: {
      StorageIoLease lease = StorageCoordinator::instance().acquireIo(
          StorageOwner::recovery, StorageAccess::read, 0);
      if (!lease) return false;
      file_.close();
      recoveryPhase_ = recoveryCandidate_
          ? (recoveryAudioCommitted_ ? RecoveryPhase::startFinalize
             : (recoveryActualBytes_ == recoveryDataBytes_
                 ? RecoveryPhase::openPatch
                 : RecoveryPhase::truncatePartial))
          : RecoveryPhase::quarantineDestinationExists;
      return false;
    }
    case RecoveryPhase::truncatePartial: {
      StorageIoLease lease = StorageCoordinator::instance().acquireIo(
          StorageOwner::recovery, StorageAccess::mutation, 0);
      if (!lease) return false;
      const String mountedPath = String("/sdcard") + partialPath_;
      if (::truncate(mountedPath.c_str(), static_cast<off_t>(
              kWavHeaderBytes + recoveryDataBytes_)) != 0) {
        if (++finalizePrimitiveFailures_ < 3) return false;
        finalizePrimitiveFailures_ = 0;
        return quarantine();
      }
      finalizePrimitiveFailures_ = 0;
      recoveryPhase_ = RecoveryPhase::openPatch;
      return false;
    }
    case RecoveryPhase::openPatch: {
      StorageIoLease lease = StorageCoordinator::instance().acquireIo(
          StorageOwner::recovery, StorageAccess::mutation, 0);
      if (!lease) return false;
      file_ = fs_->open(partialPath_, "r+");
      recoveryFileAccess_ = StorageAccess::mutation;
      if (!file_) {
        if (++finalizePrimitiveFailures_ < 3) return false;
        finalizePrimitiveFailures_ = 0;
        return quarantine();
      }
      finalizePrimitiveFailures_ = 0;
      recoveryPhase_ = RecoveryPhase::seekPatch;
      return false;
    }
    case RecoveryPhase::seekPatch: {
      StorageIoLease lease = StorageCoordinator::instance().acquireIo(
          StorageOwner::recovery, StorageAccess::mutation, 0);
      if (!lease) return false;
      if (!file_.seek(0)) {
        if (++finalizePrimitiveFailures_ < 3) return false;
        finalizePrimitiveFailures_ = 0;
        return quarantine();
      }
      finalizePrimitiveFailures_ = 0;
      recoveryPhase_ = RecoveryPhase::writePatch;
      return false;
    }
    case RecoveryPhase::writePatch: {
      uint8_t header[kWavHeaderBytes];
      encodeWavHeader(header, recoveryDataBytes_);
      StorageIoLease lease = StorageCoordinator::instance().acquireIo(
          StorageOwner::recovery, StorageAccess::mutation, 0);
      if (!lease) return false;
      if (file_.write(header, sizeof(header)) != sizeof(header) ||
          file_.getWriteError() != 0) {
        if (++finalizePrimitiveFailures_ < 3) return false;
        finalizePrimitiveFailures_ = 0;
        return quarantine();
      }
      finalizePrimitiveFailures_ = 0;
      recoveryPhase_ = RecoveryPhase::flushPatch;
      return false;
    }
    case RecoveryPhase::flushPatch: {
      StorageIoLease lease = StorageCoordinator::instance().acquireIo(
          StorageOwner::recovery, StorageAccess::mutation, 0);
      if (!lease) return false;
      file_.flush();
      if (file_.getWriteError() != 0) {
        if (++finalizePrimitiveFailures_ < 3) return false;
        finalizePrimitiveFailures_ = 0;
        return quarantine();
      }
      finalizePrimitiveFailures_ = 0;
      recoveryPhase_ = RecoveryPhase::closePatch;
      return false;
    }
    case RecoveryPhase::closePatch: {
      StorageIoLease lease = StorageCoordinator::instance().acquireIo(
          StorageOwner::recovery, StorageAccess::mutation, 0);
      if (!lease) return false;
      const int errorBefore = file_ ? file_.getWriteError() : 1;
      file_.close();
      if (errorBefore != 0 || file_.getWriteError() != 0) {
        return quarantine();
      }
      recoveryPhase_ = RecoveryPhase::startFinalize;
      return false;
    }
    case RecoveryPhase::startFinalize:
      recordingId_ = recoveryId_;
      createdAt_ = recoveryCheckpoint_.createdAt;
      directory_ = recoveryDirectory_;
      finalPath_ = directory_ + "/audio.wav";
      dataBytes_ = recoveryDataBytes_;
      checkpoint_ = recoveryCheckpoint_;
      checkpointInitialized_ = true;
      finalizeStopReason_ = RecorderStopReason::none;
      finalizePrimitiveFailures_ = 0;
      finalizePhase_ = recoveryAudioCommitted_
          ? FinalizePhase::startCapsuleMetadata
          : FinalizePhase::startAudioAndProcessing;
      finalizePending_ = true;
      recoveryFinalizeActive_ = true;
      return false;
    case RecoveryPhase::quarantineCloseFile: {
      StorageIoLease lease = StorageCoordinator::instance().acquireIo(
          StorageOwner::recovery, recoveryFileAccess_, 0);
      if (!lease) return false;
      file_.close();
      recoveryPhase_ = RecoveryPhase::quarantineDestinationExists;
      return false;
    }
    case RecoveryPhase::quarantineDestinationExists: {
      const String destination = recoveryDirectory_ + ".blocked" +
          (recoveryQuarantineSuffix_ == 0
               ? String() : "." + String(recoveryQuarantineSuffix_));
      StorageIoLease lease = StorageCoordinator::instance().acquireIo(
          StorageOwner::recovery, StorageAccess::read, 0);
      if (!lease) return false;
      if (fs_->exists(destination)) {
        ++recoveryQuarantineSuffix_;
      } else {
        recoveryPhase_ = RecoveryPhase::quarantineRename;
      }
      return false;
    }
    case RecoveryPhase::quarantineRename: {
      const String destination = recoveryDirectory_ + ".blocked" +
          (recoveryQuarantineSuffix_ == 0
               ? String() : "." + String(recoveryQuarantineSuffix_));
      StorageIoLease lease = StorageCoordinator::instance().acquireIo(
          StorageOwner::recovery, StorageAccess::mutation, 0);
      if (!lease) return false;
      if (!fs_->rename(recoveryDirectory_, destination)) {
        if (++finalizePrimitiveFailures_ < 3) return false;
        finalizePrimitiveFailures_ = 0;
        return fail();
      }
      finalizePrimitiveFailures_ = 0;
      log.printf(
          "{\"event\":\"recording_recovery_quarantined\",\"id\":\"%s\"}\n",
          recoveryId_.c_str());
      recoveryPhase_ = RecoveryPhase::scanNext;
      return false;
    }
    case RecoveryPhase::failureCloseFile: {
      StorageIoLease lease = StorageCoordinator::instance().acquireIo(
          StorageOwner::recovery, recoveryFileAccess_, 0);
      if (!lease) return false;
      file_.close();
      recoveryPhase_ = recoveryEntry_ ? RecoveryPhase::failureCloseEntry
          : (recoveryRoot_ ? RecoveryPhase::failureCloseRoot
                           : RecoveryPhase::failureDone);
      return false;
    }
    case RecoveryPhase::failureCloseEntry: {
      StorageIoLease lease = StorageCoordinator::instance().acquireIo(
          StorageOwner::recovery, StorageAccess::read, 0);
      if (!lease) return false;
      recoveryEntry_.close();
      recoveryPhase_ = recoveryRoot_ ? RecoveryPhase::failureCloseRoot
                                     : RecoveryPhase::failureDone;
      return false;
    }
    case RecoveryPhase::failureCloseRoot: {
      StorageIoLease lease = StorageCoordinator::instance().acquireIo(
          StorageOwner::recovery, StorageAccess::read, 0);
      if (!lease) return false;
      recoveryRoot_.close();
      recoveryPhase_ = RecoveryPhase::failureDone;
      return false;
    }
    case RecoveryPhase::failureDone:
      storageReservation_.release();
      bootRecoveryPending_ = false;
      bootRecoveryFailed_ = true;
      recoveryPhase_ = RecoveryPhase::failed;
      log.println("{\"event\":\"recording_boot_recovery_failed\"}");
      return false;
    case RecoveryPhase::done:
      storageReservation_.release();
      bootRecoveryPending_ = false;
      bootRecoveryReady_ = true;
      bootRecoveryFailed_ = false;
      return true;
    case RecoveryPhase::failed:
      return false;
  }
  return fail();
}

bool WavRecorder::pollStorage(Print &log, uint32_t nowMs,
                              CapsuleTransactionGate *gate) {
  if (bootRecoveryPending_ && !recoveryFinalizeActive_) {
    return pollBootRecovery(log, nowMs);
  }
  if (cleanupPending_) return pollCleanup(log, nowMs, gate);
  if (!finalizePending_) return true;
  consumeStorageQualificationInvalidations();

  switch (finalizePhase_) {
    case FinalizePhase::flushAudio: {
      StorageIoLease lease = StorageCoordinator::instance().acquireIo(
          activeStorageOwner(), StorageAccess::mutation, 0);
      if (!lease) return false;
      file_.flush();
      if (file_.getWriteError() != 0) {
        return finalizeFailure(log, RecorderTerminal::storageFailure,
                               RecorderFailureStage::flushAudio);
      }
      finalizePhase_ = FinalizePhase::seekHeader;
      return false;
    }
    case FinalizePhase::seekHeader: {
      StorageIoLease lease = StorageCoordinator::instance().acquireIo(
          activeStorageOwner(), StorageAccess::mutation, 0);
      if (!lease) return false;
      if (!file_.seek(0)) {
        return finalizeFailure(log, RecorderTerminal::headerFailure,
                               RecorderFailureStage::finalHeader);
      }
      finalizePhase_ = FinalizePhase::writeHeader;
      return false;
    }
    case FinalizePhase::writeHeader: {
      StorageIoLease lease = StorageCoordinator::instance().acquireIo(
          activeStorageOwner(), StorageAccess::mutation, 0);
      if (!lease) return false;
      if (!writeHeader(dataBytes_)) {
        return finalizeFailure(log, RecorderTerminal::headerFailure,
                               RecorderFailureStage::finalHeader);
      }
      finalizePhase_ = FinalizePhase::finalFlush;
      return false;
    }
    case FinalizePhase::finalFlush: {
      StorageIoLease lease = StorageCoordinator::instance().acquireIo(
          activeStorageOwner(), StorageAccess::mutation, 0);
      if (!lease) return false;
      file_.flush();
      if (file_.getWriteError() != 0) {
        return finalizeFailure(log, RecorderTerminal::storageFailure,
                               RecorderFailureStage::flushAudio);
      }
      finalizePhase_ = FinalizePhase::closeAudio;
      return false;
    }
    case FinalizePhase::closeAudio: {
      StorageIoLease lease = StorageCoordinator::instance().acquireIo(
          activeStorageOwner(), StorageAccess::mutation, 0);
      if (!lease) return false;
      file_.close();
      if (file_.getWriteError() != 0) {
        return finalizeFailure(log, RecorderTerminal::commitFailure,
                               RecorderFailureStage::commitAudio);
      }
      if (dataBytes_ == 0) {
        return finalizeFailure(log, RecorderTerminal::tooShort,
                               RecorderFailureStage::emptyAudio);
      }
      finalizePhase_ = FinalizePhase::startAudioAndProcessing;
      return false;
    }
    case FinalizePhase::startAudioAndProcessing:
      processingMetadata_ = processingJson(recordingId_, durationMs(),
                                           "queued", 2, nullptr, nullptr);
      processingSource_.bind(processingMetadata_);
      finalizeInputs_[0] = {finalPath_, nullptr, partialPath_};
      finalizeInputs_[1] = {directory_ + "/processing.json",
                            &processingSource_, String()};
      if (!transactionRunner_.startCommit(recordingId_.c_str(),
                                          finalizeInputs_, 2,
                                          activeStorageOwner())) {
        return finalizeFailure(log, RecorderTerminal::commitFailure,
                               RecorderFailureStage::commitAudio);
      }
      finalizePhase_ = FinalizePhase::pollAudioAndProcessing;
      return false;
    case FinalizePhase::pollAudioAndProcessing:
    case FinalizePhase::pollCapsuleMetadata:
      return pollFinalizeRunner(log, nowMs, gate);
    case FinalizePhase::startCapsuleMetadata:
      capsuleMetadata_ = capsuleJson(recordingId_, createdAt_);
      capsuleSource_.bind(capsuleMetadata_);
      finalizeInputs_[0] = {directory_ + "/capsule.json",
                            &capsuleSource_, String()};
      if (!transactionRunner_.startCommit(recordingId_.c_str(),
                                          finalizeInputs_, 1,
                                          activeStorageOwner())) {
        return finalizeFailure(log, RecorderTerminal::metadataFailure,
                               RecorderFailureStage::capsuleMetadata);
      }
      finalizePhase_ = FinalizePhase::pollCapsuleMetadata;
      return false;
    case FinalizePhase::checkInboxDirectory: {
      StorageIoLease lease = StorageCoordinator::instance().acquireIo(
          activeStorageOwner(), StorageAccess::read, 0);
      if (!lease) return false;
      inboxDirectory_ = String(kCapsuleInbox) + "/" + recordingId_;
      if (fs_->exists(inboxDirectory_)) {
        return finalizeFailure(log, RecorderTerminal::commitFailure,
                               RecorderFailureStage::commitDirectory);
      }
      finalizePhase_ = FinalizePhase::renameStagingDirectory;
      return false;
    }
    case FinalizePhase::renameStagingDirectory: {
      StorageIoLease lease = StorageCoordinator::instance().acquireIo(
          activeStorageOwner(), StorageAccess::mutation, 0);
      if (!lease) return false;
      if (!fs_->rename(directory_, inboxDirectory_)) {
        if (++finalizePrimitiveFailures_ < 3) return false;
        return finalizeFailure(log, RecorderTerminal::commitFailure,
                               RecorderFailureStage::commitDirectory);
      }
      finalizePrimitiveFailures_ = 0;
      directory_ = inboxDirectory_;
      finalPath_ = directory_ + "/audio.wav";
      finalizePhase_ = FinalizePhase::checkCheckpoint;
      return false;
    }
    case FinalizePhase::checkCheckpoint: {
      StorageIoLease lease = StorageCoordinator::instance().acquireIo(
          activeStorageOwner(), StorageAccess::read, 0);
      if (!lease) return false;
      const String path = directory_ + "/recording.chk";
      finalizePhase_ = fs_->exists(path) ? FinalizePhase::removeCheckpoint
                                         : FinalizePhase::complete;
      return false;
    }
    case FinalizePhase::removeCheckpoint: {
      StorageIoLease lease = StorageCoordinator::instance().acquireIo(
          activeStorageOwner(), StorageAccess::mutation, 0);
      if (!lease) return false;
      const String path = directory_ + "/recording.chk";
      if (!fs_->remove(path)) {
        if (++finalizePrimitiveFailures_ < 3) return false;
        log.println("{\"event\":\"recording_warning\",\"stage\":\"checkpoint_cleanup\"}");
        finalizePrimitiveFailures_ = 0;
        finalizePhase_ = FinalizePhase::quarantineCheckpoint;
        return false;
      }
      finalizePrimitiveFailures_ = 0;
      finalizePhase_ = FinalizePhase::complete;
      return false;
    }
    case FinalizePhase::quarantineCheckpoint: {
      StorageIoLease lease = StorageCoordinator::instance().acquireIo(
          activeStorageOwner(), StorageAccess::mutation, 0);
      if (!lease) return false;
      const String path = directory_ + "/recording.chk";
      const String stale = directory_ + "/recording.chk.stale";
      if (!fs_->rename(path, stale)) {
        if (++finalizePrimitiveFailures_ < 3) return false;
        log.println(
            "{\"event\":\"recording_cleanup_blocked\",\"artifact\":\"success_checkpoint\"}");
      }
      finalizePrimitiveFailures_ = 0;
      finalizePhase_ = FinalizePhase::complete;
      return false;
    }
    case FinalizePhase::complete:
      completeFinalize(log);
      return true;
    case FinalizePhase::idle:
      return true;
  }
  return finalizeFailure(log, RecorderTerminal::commitFailure,
                         RecorderFailureStage::commitAudio);
}

bool WavRecorder::pollFinalize(Print &log, uint32_t nowMs,
                               CapsuleTransactionGate *gate) {
#if defined(ARDUINO_ARCH_ESP32)
  // Boot recovery's reservation is created by the Arduino task, so every
  // recovery primitive (including a recovered recording's finalize) remains
  // on that same context before capture is admitted.
  if (bootRecoveryPending_) {
    return pollStorage(log, nowMs, nullptr);
  }
  storageNowMs_.store(nowMs, std::memory_order_release);
  if (operationOwner_.load(std::memory_order_acquire) ==
      RecorderOperationOwner::linkWifi) {
    storageGateObserved_.store(true, std::memory_order_release);
    if (!capsuleTransactionPermitted(gate, nowMs)) {
      storageCancellationGate_.cancel();
    }
  }
  if (recording_ || finalizePending_ || cleanupPending_ ||
      storageSessionActive_.load(std::memory_order_acquire) ||
      storageStartRequested_.load(std::memory_order_acquire)) {
    storageSessionActive_.store(true, std::memory_order_release);
    xTaskNotifyGive(storageTask_);
    return false;
  }
  return true;
#else
  return pollStorage(log, nowMs, gate);
#endif
}

void WavRecorder::resetSessionState() {
  // An open handle is owned by either the active recording or its deferred
  // cleanup.  Closing it here would bypass the StorageCoordinator lease.
  if (file_ || cleanupPending_ || finalizePending_) return;
  recording_ = false;
  captureFailureCode_.store(0, std::memory_order_release);
  operationOwner_ = RecorderOperationOwner::none;
  recordingId_ = "";
  createdAt_ = "";
  directory_ = "";
  partialPath_ = "";
  finalPath_ = "";
  dataBytes_ = 0;
  automaticStopRequested_ = false;
  automaticStopReason_ = RecorderStopReason::none;
  finalizePhase_ = FinalizePhase::idle;
  finalizeStopReason_ = RecorderStopReason::none;
  finalizePrimitiveFailures_ = 0;
  processingMetadata_ = "";
  capsuleMetadata_ = "";
  inboxDirectory_ = "";
  checkpointInitialized_ = false;
  checkpointCrcState_ = recorderAudioCrc32Begin();
  checkpointedBytes_ = 0;
  memset(&checkpoint_, 0, sizeof(checkpoint_));
#if defined(ARDUINO_ARCH_ESP32)
  storageQueue_.reset();
  acceptedDataBytes_.store(0, std::memory_order_release);
  periodicCheckpointPhase_ = PeriodicCheckpointPhase::idle;
#endif
  storageReservation_.release();
  terminalState_.reset();
  audioMetrics_ = {};
  audioMetricsSessionId_ = 0;
  audioMetricsGeneration_ = 0;
  activeMountGeneration_ = 0;
  activeQualificationProbeEpoch_ = 0;
  sessionTelemetry_.reset();
  captureTelemetryProducer_ = {};
#if defined(ARDUINO_ARCH_ESP32)
  recorderTelemetryLastStackSampleMs_.store(0, std::memory_order_release);
  recorderTelemetryStackHighWaterWords_.store(0, std::memory_order_release);
  recorderTelemetryStackSampled_.store(false, std::memory_order_release);
#endif
}

bool WavRecorder::finishFailure(Print &log, RecorderTerminal terminal,
                                RecorderFailureStage stage) {
  recordRuntime(RuntimeDiagnosticStage::recordingFailure,
                RuntimeDiagnosticOutcome::failure,
                static_cast<uint32_t>(stage), static_cast<uint32_t>(terminal),
                log);
  recording_ = false;
  automaticStopRequested_ = false;
  automaticStopReason_ = RecorderStopReason::none;
  finalizePending_ = false;
  finalizePhase_ = FinalizePhase::idle;
  if (stage == RecorderFailureStage::shortWrite ||
      stage == RecorderFailureStage::flushAudio ||
      stage == RecorderFailureStage::openAudio ||
      stage == RecorderFailureStage::initialHeader ||
      stage == RecorderFailureStage::finalHeader) {
    storageQualification_.invalidateOwned(
        stage == RecorderFailureStage::shortWrite
            ? RecordingQualificationInvalidReason::shortWrite
            : RecordingQualificationInvalidReason::ioError);
  }
  if (terminal == RecorderTerminal::storageFailure ||
      terminal == RecorderTerminal::headerFailure ||
      terminal == RecorderTerminal::metadataFailure ||
      terminal == RecorderTerminal::commitFailure ||
      terminal == RecorderTerminal::cleanupBlocked) {
    storageQualification_.invalidateOwned(
        RecordingQualificationInvalidReason::ioError);
  }
  if (!cleanupPending_) {
    cleanupPending_ = true;
    cleanupPhase_ = file_ ? CleanupPhase::flushAudio
                          : CleanupPhase::startFailureCheckpoint;
    cleanupPrimitiveFailures_ = 0;
    cleanupTerminal_ = terminal;
    cleanupStage_ = stage;
  }
  return false;
}

bool WavRecorder::pollCleanup(Print &log, uint32_t nowMs,
                              CapsuleTransactionGate *gate) {
  if (!cleanupPending_) return true;
  if (cleanupPhase_ == CleanupPhase::flushAudio) {
    if (!file_) {
      cleanupPhase_ = CleanupPhase::startFailureCheckpoint;
      return false;
    }
    StorageIoLease lease = StorageCoordinator::instance().acquireIo(
        activeStorageOwner(), StorageAccess::mutation, 0);
    if (!lease) return false;
    file_.flush();
    cleanupPhase_ = CleanupPhase::closeAudio;
    return false;
  }
  if (cleanupPhase_ == CleanupPhase::closeAudio) {
    if (file_) {
      StorageIoLease lease = StorageCoordinator::instance().acquireIo(
          activeStorageOwner(), StorageAccess::mutation, 0);
      if (!lease) return false;
      file_.close();
    }
    cleanupPhase_ = CleanupPhase::startFailureCheckpoint;
    return false;
  }
  if (cleanupPhase_ == CleanupPhase::startFailureCheckpoint) {
    if (!checkpointInitialized_ || directory_.isEmpty()) {
      cleanupPhase_ = directory_.isEmpty()
          ? CleanupPhase::publishTerminal
          : CleanupPhase::removeEmptyStaging;
      return false;
    }
    updateRecorderCheckpoint(checkpoint_, dataBytes_,
                             recorderAudioCrc32Finish(checkpointCrcState_));
    failRecorderCheckpoint(checkpoint_, cleanupStage_);
    failureCheckpointSource_.bind(checkpoint_);
    finalizeInputs_[0] = {directory_ + "/recording.failed.chk",
                          &failureCheckpointSource_, String()};
    const String failureKey = recordingId_ + "-failure";
    if (!transactionRunner_.startCommit(failureKey.c_str(),
                                        finalizeInputs_, 1,
                                        activeStorageOwner())) {
      log.println(
          "{\"event\":\"recording_warning\",\"stage\":\"failed_checkpoint_start\"}");
      if (cleanupTerminal_ != RecorderTerminal::cancelled) {
        cleanupTerminal_ = RecorderTerminal::cleanupBlocked;
      }
      failureMarkerPath_ = directory_ + "/recording.failure";
      cleanupPhase_ = CleanupPhase::openFailureMarker;
      return false;
    }
    cleanupPhase_ = CleanupPhase::pollFailureCheckpoint;
    return false;
  }
  if (cleanupPhase_ == CleanupPhase::removeEmptyStaging) {
    StorageIoLease lease = StorageCoordinator::instance().acquireIo(
        activeStorageOwner(), StorageAccess::mutation, 0);
    if (!lease) return false;
    if (!fs_->rmdir(directory_)) {
      log.println(
          "{\"event\":\"recording_cleanup_blocked\",\"artifact\":\"empty_staging\"}");
      cleanupTerminal_ = RecorderTerminal::cleanupBlocked;
    }
    cleanupPhase_ = CleanupPhase::publishTerminal;
    return false;
  }
  if (cleanupPhase_ == CleanupPhase::pollFailureCheckpoint) {
    const CapsuleTransactionPollResult result =
        transactionRunner_.poll(nowMs, gate);
    if (result == CapsuleTransactionPollResult::progress ||
        result == CapsuleTransactionPollResult::wouldBlock) {
      return false;
    }
    if (result != CapsuleTransactionPollResult::committed) {
      log.println(
          "{\"event\":\"recording_warning\",\"stage\":\"failed_checkpoint_commit\"}");
      if (cleanupTerminal_ != RecorderTerminal::cancelled) {
        cleanupTerminal_ = RecorderTerminal::cleanupBlocked;
      }
      failureMarkerPath_ = directory_ + "/recording.failure";
      cleanupPrimitiveFailures_ = 0;
      cleanupPhase_ = CleanupPhase::openFailureMarker;
      return false;
    }
    cleanupPhase_ = CleanupPhase::publishTerminal;
    return false;
  }
  if (cleanupPhase_ == CleanupPhase::openFailureMarker) {
    StorageIoLease lease = StorageCoordinator::instance().acquireIo(
        activeStorageOwner(), StorageAccess::mutation, 0);
    if (!lease) return false;
    file_ = fs_->open(failureMarkerPath_, FILE_WRITE);
    if (!file_) {
      if (++cleanupPrimitiveFailures_ < 3) return false;
      cleanupPrimitiveFailures_ = 0;
      cleanupPhase_ = CleanupPhase::renameFailureCheckpoint;
      return false;
    }
    cleanupPrimitiveFailures_ = 0;
    cleanupPhase_ = CleanupPhase::writeFailureMarker;
    return false;
  }
  if (cleanupPhase_ == CleanupPhase::writeFailureMarker) {
    StorageIoLease lease = StorageCoordinator::instance().acquireIo(
        activeStorageOwner(), StorageAccess::mutation, 0);
    if (!lease) return false;
    const size_t written = file_.write(
        reinterpret_cast<const uint8_t *>(&checkpoint_),
        sizeof(checkpoint_));
    if (written != sizeof(checkpoint_) || file_.getWriteError() != 0) {
      if (++cleanupPrimitiveFailures_ < 3) return false;
      cleanupPrimitiveFailures_ = 0;
      cleanupPhase_ = CleanupPhase::closeFailureMarker;
      return false;
    }
    cleanupPrimitiveFailures_ = 0;
    cleanupPhase_ = CleanupPhase::flushFailureMarker;
    return false;
  }
  if (cleanupPhase_ == CleanupPhase::flushFailureMarker) {
    StorageIoLease lease = StorageCoordinator::instance().acquireIo(
        activeStorageOwner(), StorageAccess::mutation, 0);
    if (!lease) return false;
    file_.flush();
    if (file_.getWriteError() != 0) {
      if (++cleanupPrimitiveFailures_ < 3) return false;
      cleanupPrimitiveFailures_ = 0;
    }
    cleanupPhase_ = CleanupPhase::closeFailureMarker;
    return false;
  }
  if (cleanupPhase_ == CleanupPhase::closeFailureMarker) {
    StorageIoLease lease = StorageCoordinator::instance().acquireIo(
        activeStorageOwner(), StorageAccess::mutation, 0);
    if (!lease) return false;
    const int errorBefore = file_ ? file_.getWriteError() : 1;
    if (file_) file_.close();
    const int errorAfter = file_.getWriteError();
    cleanupPhase_ = errorBefore == 0 && errorAfter == 0
        ? CleanupPhase::openFailureMarkerReadback
        : CleanupPhase::renameFailureCheckpoint;
    return false;
  }
  if (cleanupPhase_ == CleanupPhase::openFailureMarkerReadback) {
    StorageIoLease lease = StorageCoordinator::instance().acquireIo(
        activeStorageOwner(), StorageAccess::read, 0);
    if (!lease) return false;
    file_ = fs_->open(failureMarkerPath_, FILE_READ);
    if (!file_) {
      cleanupPhase_ = CleanupPhase::renameFailureCheckpoint;
      return false;
    }
    failureMarkerReadbackValid_ = false;
    cleanupPhase_ = CleanupPhase::readFailureMarker;
    return false;
  }
  if (cleanupPhase_ == CleanupPhase::readFailureMarker) {
    StorageIoLease lease = StorageCoordinator::instance().acquireIo(
        activeStorageOwner(), StorageAccess::read, 0);
    if (!lease) return false;
    const int bytes = file_.read(
        reinterpret_cast<uint8_t *>(&failureMarkerReadback_),
        sizeof(failureMarkerReadback_));
    failureMarkerReadbackValid_ =
        bytes == static_cast<int>(sizeof(failureMarkerReadback_)) &&
        memcmp(&failureMarkerReadback_, &checkpoint_, sizeof(checkpoint_)) == 0 &&
        validateRecorderCheckpoint(failureMarkerReadback_) &&
        failureMarkerReadback_.state ==
            static_cast<uint8_t>(RecorderCheckpointState::failed);
    cleanupPhase_ = CleanupPhase::closeFailureMarkerReadback;
    return false;
  }
  if (cleanupPhase_ == CleanupPhase::closeFailureMarkerReadback) {
    StorageIoLease lease = StorageCoordinator::instance().acquireIo(
        activeStorageOwner(), StorageAccess::read, 0);
    if (!lease) return false;
    file_.close();
    cleanupPhase_ = failureMarkerReadbackValid_
        ? CleanupPhase::publishTerminal
        : CleanupPhase::renameFailureCheckpoint;
    return false;
  }
  if (cleanupPhase_ == CleanupPhase::renameFailureCheckpoint) {
    StorageIoLease lease = StorageCoordinator::instance().acquireIo(
        activeStorageOwner(), StorageAccess::mutation, 0);
    if (!lease) return false;
    const String checkpointPath = directory_ + "/recording.chk";
    const String failedPath = directory_ + "/recording.failure.chk";
    if (!fs_->rename(checkpointPath, failedPath)) {
      if (++cleanupPrimitiveFailures_ < 3) return false;
      log.println(
          "{\"event\":\"recording_storage_degraded\",\"artifact\":\"failure_marker\",\"staging_preserved\":true}");
      cleanupTerminal_ = RecorderTerminal::cleanupBlocked;
      cleanupPrimitiveFailures_ = 0;
      if (recoveryFinalizeActive_) {
        cleanupPending_ = false;
        recoveryFinalizeActive_ = false;
        finalizePending_ = false;
        finalizePhase_ = FinalizePhase::idle;
        recoveryPhase_ = recoveryRoot_ ? RecoveryPhase::failureCloseRoot
                                       : RecoveryPhase::failureDone;
        return false;
      }
      // No durable failure fact can be created while both writes and the
      // atomic checkpoint rename are unavailable.  Preserve the staging
      // evidence, publish a bounded terminal result, and fail this recorder
      // instance closed so a new session cannot reinterpret the old
      // recording checkpoint as reusable truth.  A reboot will run the
      // normal recovery/quarantine path before recording is enabled.
      bootRecoveryFailed_ = true;
      cleanupPhase_ = CleanupPhase::publishTerminal;
      return false;
    }
    cleanupPrimitiveFailures_ = 0;
    // The rename makes the old recording truth non-upgradable while writes
    // are unavailable.  Publication still waits for a failed checkpoint with
    // the current byte count and failure stage to commit and read back.
    cleanupPhase_ = CleanupPhase::startFailureCheckpoint;
    return false;
  }
  if (cleanupPhase_ != CleanupPhase::publishTerminal) return false;
  if (recoveryFinalizeActive_) {
    cleanupPending_ = false;
    cleanupPhase_ = CleanupPhase::idle;
    cleanupPrimitiveFailures_ = 0;
    cleanupTerminal_ = RecorderTerminal::none;
    cleanupStage_ = RecorderFailureStage::none;
    finalizePending_ = false;
    finalizePhase_ = FinalizePhase::idle;
    recoveryFinalizeActive_ = false;
    recoveryPhase_ = RecoveryPhase::scanNext;
    recordingId_ = "";
    createdAt_ = "";
    directory_ = "";
    partialPath_ = "";
    finalPath_ = "";
    dataBytes_ = 0;
    checkpointInitialized_ = false;
    return false;
  }
  storageReservation_.release();
  terminalState_.fail(cleanupTerminal_, cleanupStage_, dataBytes_);
  updateRecorderTelemetry(true);
  freezeSessionTelemetry(log);
  log.printf("{\"event\":\"recording_terminal\",\"ok\":false,\"stage\":\"%s\",\"bytes\":%lu}\n",
             recorderFailureStageName(cleanupStage_),
             static_cast<unsigned long>(dataBytes_));
  cleanupPending_ = false;
  cleanupPhase_ = CleanupPhase::idle;
  cleanupPrimitiveFailures_ = 0;
  cleanupTerminal_ = RecorderTerminal::none;
  cleanupStage_ = RecorderFailureStage::none;
  operationOwner_ = RecorderOperationOwner::none;
  return true;
}

bool WavRecorder::ensureDirectory(const char *path, Print &log) {
  StorageIoLease lease = StorageCoordinator::instance().acquireIo(
      activeStorageOwner(), StorageAccess::mutation, storageIoTimeoutMs());
  if (!lease) return false;
  if (fs_->exists(path) || fs_->mkdir(path)) return true;
  log.printf("{\"event\":\"storage_error\",\"stage\":\"mkdir\",\"path\":\"%s\"}\n", path);
  return false;
}

bool WavRecorder::writeTextAtomically(const String &finalPath,
                                      const String &text, Print &log) {
  const bool ok = transaction_.writeTextAtomic(
      finalPath, text, activeStorageOwner(), "recorder-metadata");
  if (!ok) {
    log.println("{\"event\":\"storage_error\",\"stage\":\"transaction_write\"}");
  }
  return ok;
}

bool WavRecorder::writeHeader(uint32_t dataBytes) {
  if (!file_) return false;
  uint8_t header[kWavHeaderBytes];
  encodeWavHeader(header, dataBytes);
  return file_.write(header, sizeof(header)) == sizeof(header) &&
      file_.getWriteError() == 0;
}

bool WavRecorder::writeProcessingMetadata(Print &log, const char *status,
                                          uint32_t revision,
                                          const char *errorStage,
                                          const char *error) {
  return writeTextAtomically(directory_ + "/processing.json",
                             processingJson(recordingId_, durationMs(), status,
                                            revision, errorStage, error), log);
}

StorageOwner WavRecorder::activeStorageOwner() const {
  return storageReservation_ ? storageReservation_.owner()
                             : StorageOwner::recovery;
}

#if defined(ARDUINO_ARCH_ESP32)
bool WavRecorder::ensureStorageTask(Print &log) {
  storageLog_ = &log;
  if (storageTask_ != nullptr) {
    return storageQueueSlots_ != nullptr && recordingProbeBuffer_ != nullptr;
  }
  storageQueueSlots_ = static_cast<RecorderStorageFrame *>(heap_caps_calloc(
      kRecorderStorageQueueSlots, sizeof(RecorderStorageFrame),
      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  recordingProbeBuffer_ = static_cast<uint8_t *>(heap_caps_calloc(
      kRecordingProbeChunkBytes, 1,
      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (storageQueueSlots_ == nullptr || recordingProbeBuffer_ == nullptr ||
      !storageQueue_.bind(storageQueueSlots_, kRecorderStorageQueueSlots)) {
    if (recordingProbeBuffer_ != nullptr) heap_caps_free(recordingProbeBuffer_);
    recordingProbeBuffer_ = nullptr;
    if (storageQueueSlots_ != nullptr) heap_caps_free(storageQueueSlots_);
    storageQueueSlots_ = nullptr;
    log.println(
        "{\"event\":\"recording_storage_task\",\"ok\":false,\"stage\":\"psram_buffers\"}");
    return false;
  }
  storageStartAck_ = xSemaphoreCreateBinary();
  if (storageStartAck_ == nullptr) {
    heap_caps_free(recordingProbeBuffer_);
    recordingProbeBuffer_ = nullptr;
    heap_caps_free(storageQueueSlots_);
    storageQueueSlots_ = nullptr;
    log.println(
        "{\"event\":\"recording_storage_task\",\"ok\":false,\"stage\":\"start_ack\"}");
    return false;
  }
  const BaseType_t created = xTaskCreatePinnedToCore(
      // The admission/probe path constructs transaction and diagnostic
      // objects in the storage task before the recording session is live.
      // 4096 words was insufficient on ESP32-S3: the first successful
      // qualification publication could exhaust the task stack after the
      // SD probe had already completed. Keep the queue/bulk buffers in PSRAM,
      // but give this control path a bounded 8192-word stack.
      storageTaskThunk, "pokepod_recorder_storage", 8192, this, 1,
      &storageTask_, 0);
  if (created != pdPASS || storageTask_ == nullptr) {
    vSemaphoreDelete(storageStartAck_);
    storageStartAck_ = nullptr;
    heap_caps_free(recordingProbeBuffer_);
    recordingProbeBuffer_ = nullptr;
    heap_caps_free(storageQueueSlots_);
    storageQueueSlots_ = nullptr;
    log.println(
        "{\"event\":\"recording_storage_task\",\"ok\":false,\"stage\":\"task\"}");
    return false;
  }
  log.printf(
      "{\"event\":\"recording_storage_task\",\"ok\":true,\"queue_frames\":%u,\"queue_ms\":%u,\"memory\":\"psram\"}\n",
      static_cast<unsigned>(kRecorderStorageQueueFrames),
      static_cast<unsigned>(kRecorderStorageQueueFrames * 20U));
  return true;
}

void WavRecorder::storageTaskThunk(void *context) {
  static_cast<WavRecorder *>(context)->storageTaskMain();
}

bool WavRecorder::pollPeriodicCheckpoint(Print &log, uint32_t nowMs,
                                         CapsuleTransactionGate *gate) {
  if (periodicCheckpointPhase_ == PeriodicCheckpointPhase::flushAudio) {
    StorageIoLease lease = StorageCoordinator::instance().acquireIo(
        activeStorageOwner(), StorageAccess::mutation, 0);
    if (!lease) return false;
    file_.flush();
    if (file_.getWriteError() != 0) {
      periodicCheckpointPhase_ = PeriodicCheckpointPhase::idle;
      return finishFailure(log, RecorderTerminal::metadataFailure,
                           RecorderFailureStage::recoveryCheckpoint);
    }
    periodicCheckpointPhase_ = PeriodicCheckpointPhase::startTransaction;
    return false;
  }
  if (periodicCheckpointPhase_ ==
      PeriodicCheckpointPhase::startTransaction) {
    finalizeInputs_[0] = {directory_ + "/recording.chk",
                          &periodicCheckpointSource_, String()};
    const String key = recordingId_ + "-checkpoint";
    if (!transactionRunner_.startCommit(key.c_str(), finalizeInputs_, 1,
                                        activeStorageOwner())) {
      periodicCheckpointPhase_ = PeriodicCheckpointPhase::idle;
      return finishFailure(log, RecorderTerminal::metadataFailure,
                           RecorderFailureStage::recoveryCheckpoint);
    }
    periodicCheckpointPhase_ = PeriodicCheckpointPhase::pollTransaction;
    return false;
  }
  if (periodicCheckpointPhase_ ==
      PeriodicCheckpointPhase::pollTransaction) {
    const CapsuleTransactionPollResult result =
        transactionRunner_.poll(nowMs, gate);
    if (result == CapsuleTransactionPollResult::progress ||
        result == CapsuleTransactionPollResult::wouldBlock) return false;
    periodicCheckpointPhase_ = PeriodicCheckpointPhase::idle;
    if (result != CapsuleTransactionPollResult::committed) {
      RecorderTerminal terminal = RecorderTerminal::metadataFailure;
      if (result == CapsuleTransactionPollResult::cancelled) {
        terminal = RecorderTerminal::cancelled;
      } else if (result == CapsuleTransactionPollResult::cleanupBlocked ||
                 result == CapsuleTransactionPollResult::recoveryBlocked) {
        terminal = RecorderTerminal::cleanupBlocked;
      }
      return finishFailure(log, terminal,
                           RecorderFailureStage::recoveryCheckpoint);
    }
    checkpoint_ = periodicCheckpoint_;
    checkpointedBytes_ = periodicCheckpoint_.confirmedDataBytes;
    return true;
  }
  return true;
}

void WavRecorder::storageTaskMain() {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
    while (storageSessionActive_.load(std::memory_order_acquire)) {
      consumeStorageQualificationInvalidations();
      Print *log = storageLog_;
      if (log == nullptr) {
        vTaskDelay(pdMS_TO_TICKS(1));
        continue;
      }
      if (storageStartRequested_.exchange(false,
                                          std::memory_order_acq_rel)) {
        const bool started = startStorageSession(*log);
        storageStartSucceeded_.store(started, std::memory_order_release);
        xSemaphoreGive(storageStartAck_);
        if (!started && !cleanupPending_) {
          storageSessionActive_.store(false, std::memory_order_release);
          break;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
        continue;
      }
      if (storageAbortRequested_.exchange(false,
                                          std::memory_order_acq_rel)) {
        storageQueue_.reset();
        RecorderTerminal terminal = captureFailureTerminal();
        RecorderFailureStage stage = captureFailureStage();
        if (terminal == RecorderTerminal::none) {
          terminal = RecorderTerminal::captureFailure;
        }
        if (stage == RecorderFailureStage::none) {
          stage = RecorderFailureStage::captureIncomplete;
        }
        (void)finishFailure(*log, terminal, stage);
        vTaskDelay(pdMS_TO_TICKS(1));
        continue;
      }
      CapsuleTransactionGate *gate = nullptr;
      const bool wifi = operationOwner_.load(std::memory_order_acquire) ==
          RecorderOperationOwner::linkWifi;
      if (wifi) {
        if (!storageGateObserved_.load(std::memory_order_acquire) &&
            (periodicCheckpointPhase_ != PeriodicCheckpointPhase::idle ||
             finalizePending_ || cleanupPending_)) {
          vTaskDelay(pdMS_TO_TICKS(1));
          continue;
        }
        gate = &storageCancellationGate_;
      }

      if (cleanupPending_) {
        storageQueue_.reset();
        (void)pollStorage(*log,
                          storageNowMs_.load(std::memory_order_acquire), gate);
      } else if (periodicCheckpointPhase_ !=
                 PeriodicCheckpointPhase::idle) {
        (void)pollPeriodicCheckpoint(
            *log, storageNowMs_.load(std::memory_order_acquire), gate);
      } else if (storageQueue_.pop(storageFrame_)) {
        if (!storageAppendMonoBytes(storageFrame_.bytes,
                                    storageFrame_.length, *log)) {
          storageQueue_.reset();
        }
      } else if (finalizePending_) {
        (void)pollStorage(*log,
                          storageNowMs_.load(std::memory_order_acquire), gate);
      } else if (!recording_) {
        storageSessionActive_.store(false, std::memory_order_release);
        break;
      } else {
        break;
      }
      vTaskDelay(pdMS_TO_TICKS(1));
    }
  }
}
#endif

bool WavRecorder::persistCheckpoint(bool failed, RecorderFailureStage stage,
                                    Print &log) {
  if (!checkpointInitialized_ || directory_.isEmpty()) return false;
  if (file_) {
    StorageIoLease lease = StorageCoordinator::instance().acquireIo(
        activeStorageOwner(), StorageAccess::mutation, storageIoTimeoutMs());
    if (!lease) return false;
    file_.flush();
    if (file_.getWriteError() != 0) return false;
  }
  updateRecorderCheckpoint(checkpoint_, dataBytes_,
                           recorderAudioCrc32Finish(checkpointCrcState_));
  if (failed) failRecorderCheckpoint(checkpoint_, stage);
  const bool ok = transaction_.writeBytesAtomic(
      directory_ + "/recording.chk",
      reinterpret_cast<const uint8_t *>(&checkpoint_), sizeof(checkpoint_),
      activeStorageOwner(), recordingId_.c_str());
  if (ok) checkpointedBytes_ = checkpoint_.confirmedDataBytes;
  else log.println("{\"event\":\"recording_error\",\"stage\":\"checkpoint_write\"}");
  return ok;
}

}  // namespace pokepod
