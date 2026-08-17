#include "RuntimeDiagnostics.h"

#include <esp_heap_caps.h>
#include <esp_system.h>
#include <time.h>

namespace pokepod {
namespace {

constexpr char kRuntimeLogKey[] = "runtime_log_v1";
constexpr uint32_t kValidEpoch = 1704067200;

uint32_t internalFree() {
  return heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

uint32_t internalLargest() {
  return heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

uint32_t psramFree() {
  return heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

}  // namespace

bool RuntimeDiagnostics::begin(Print &log, uint16_t resetReason) {
#if defined(ARDUINO)
  if (mutex_ == nullptr) {
    mutex_ = xSemaphoreCreateMutexStatic(&mutexStorage_);
  }
#endif
  lock();
  audioSessionSnapshot_ = {};
  audioSessionSnapshotAvailable_ = false;
  initializeRuntimeDiagnosticTrace(trace_);
  open_ = preferences_.begin("pokepod_rt", false);
  initializeRuntimeDiagnosticLog(stored_);
  if (!open_) {
    unlock();
    log.println("{\"event\":\"runtime_log\",\"ok\":false,\"stage\":\"nvs_open\"}");
    return false;
  }
  StoredRuntimeDiagnosticLog loaded{};
  const bool loadedOk =
      preferences_.getBytesLength(kRuntimeLogKey) == sizeof(loaded) &&
      preferences_.getBytes(kRuntimeLogKey, &loaded, sizeof(loaded)) ==
          sizeof(loaded) && validateRuntimeDiagnosticLog(loaded);
  if (loadedOk) stored_ = loaded;
  StoredRuntimeDiagnosticLog proposed = stored_;
  StoredRuntimeDiagnosticRecord record{};
  const time_t now = time(nullptr);
  record.epoch = now >= kValidEpoch ? static_cast<uint32_t>(now) : 0;
  record.uptimeMs = millis();
  record.internalFree = internalFree();
  record.internalLargest = internalLargest();
  record.psramFree = psramFree();
  record.resetReason = resetReason;
  record.subsystem = static_cast<uint8_t>(RuntimeDiagnosticSubsystem::boot);
  record.stage = static_cast<uint8_t>(RuntimeDiagnosticStage::boot);
  record.outcome = static_cast<uint8_t>(RuntimeDiagnosticOutcome::success);
  appendRuntimeDiagnosticTrace(trace_, record);
  appendRuntimeDiagnostic(proposed, record);
  finalizeRuntimeDiagnosticLog(proposed);
  const bool recorded = persist(proposed);
  if (recorded) stored_ = proposed;
  const unsigned count = static_cast<unsigned>(stored_.count);
  unlock();
  log.printf("{\"event\":\"runtime_log\",\"ok\":%s,\"count\":%u,\"recovered\":%s,\"reset_reason\":%u}\n",
             recorded ? "true" : "false", count,
             loadedOk ? "true" : "false", static_cast<unsigned>(resetReason));
  return recorded;
}

bool RuntimeDiagnostics::persist(const StoredRuntimeDiagnosticLog &proposed) {
  return open_ && preferences_.putBytes(
      kRuntimeLogKey, &proposed, sizeof(proposed)) == sizeof(proposed);
}

bool RuntimeDiagnostics::record(RuntimeDiagnosticSubsystem subsystem,
                                RuntimeDiagnosticStage stage,
                                RuntimeDiagnosticOutcome outcome,
                                uint32_t detail0, uint32_t detail1,
                                Print &log) {
  lock();
  StoredRuntimeDiagnosticRecord record{};
  const time_t now = time(nullptr);
  record.epoch = now >= kValidEpoch ? static_cast<uint32_t>(now) : 0;
  record.uptimeMs = millis();
  record.detail0 = detail0;
  record.detail1 = detail1;
  record.internalFree = internalFree();
  record.internalLargest = internalLargest();
  record.psramFree = psramFree();
  record.resetReason = static_cast<uint16_t>(esp_reset_reason());
  record.subsystem = static_cast<uint8_t>(subsystem);
  record.stage = static_cast<uint8_t>(stage);
  record.outcome = static_cast<uint8_t>(outcome);
  appendRuntimeDiagnosticTrace(trace_, record);
  const RuntimeDiagnosticTraceRecord *latestTrace =
      runtimeDiagnosticTraceNewest(trace_, 0);
  const uint32_t traceSequence =
      latestTrace != nullptr ? latestTrace->traceSequence : 0;
  const bool persistent = runtimeDiagnosticShouldPersist(
      subsystem, stage, outcome);
  bool ok = true;
  if (persistent) {
    StoredRuntimeDiagnosticLog proposed = stored_;
    appendRuntimeDiagnostic(proposed, record);
    finalizeRuntimeDiagnosticLog(proposed);
    ok = open_ && persist(proposed);
    if (ok) stored_ = proposed;
  }
  unlock();
  log.printf("{\"event\":\"runtime_diagnostic\",\"ok\":%s,\"persistent\":%s,\"trace_sequence\":%lu,\"subsystem\":\"%s\",\"stage\":\"%s\",\"outcome\":\"%s\",\"detail0\":%lu,\"detail1\":%lu,\"internal_free\":%lu,\"internal_largest\":%lu,\"psram_free\":%lu,\"reset_reason\":%u}\n",
             ok ? "true" : "false", persistent ? "true" : "false",
             static_cast<unsigned long>(traceSequence),
             runtimeDiagnosticSubsystemKey(subsystem),
             runtimeDiagnosticStageKey(stage),
             runtimeDiagnosticOutcomeKey(outcome),
             static_cast<unsigned long>(detail0),
             static_cast<unsigned long>(detail1),
             static_cast<unsigned long>(record.internalFree),
             static_cast<unsigned long>(record.internalLargest),
             static_cast<unsigned long>(record.psramFree),
             static_cast<unsigned>(record.resetReason));
  return ok;
}

bool RuntimeDiagnostics::clear(Print &log) {
  lock();
  audioSessionSnapshot_ = {};
  audioSessionSnapshotAvailable_ = false;
  initializeRuntimeDiagnosticTrace(trace_);
  StoredRuntimeDiagnosticLog proposed{};
  initializeRuntimeDiagnosticLog(proposed);
  finalizeRuntimeDiagnosticLog(proposed);
  const bool ok = persist(proposed);
  if (ok) stored_ = proposed;
  unlock();
  log.printf("{\"event\":\"runtime_log_cleared\",\"ok\":%s}\n",
             ok ? "true" : "false");
  return ok;
}

String RuntimeDiagnostics::json() const {
  const_cast<RuntimeDiagnostics *>(this)->lock();
  const StoredRuntimeDiagnosticLog copy = stored_;
  const AudioSessionTelemetrySnapshot audioSnapshot = audioSessionSnapshot_;
  const bool audioSnapshotAvailable = audioSessionSnapshotAvailable_;
  const bool open = open_;
  const_cast<RuntimeDiagnostics *>(this)->unlock();
  String json;
  json.reserve(4096);
  json = "{\"status\":\"";
  json += open ? "ok" : "unavailable";
  json += "\",\"version\":1,\"count\":";
  json += String(static_cast<unsigned>(copy.count));
  json += ",\"records\":[";
  for (size_t offset = 0; offset < copy.count; ++offset) {
    const StoredRuntimeDiagnosticRecord *record =
        runtimeDiagnosticNewest(copy, offset);
    if (record == nullptr) continue;
    if (offset != 0) json += ',';
    json += "{\"sequence\":";
    json += String(static_cast<unsigned long>(record->sequence));
    json += ",\"epoch\":";
    json += String(static_cast<unsigned long>(record->epoch));
    json += ",\"uptime_ms\":";
    json += String(static_cast<unsigned long>(record->uptimeMs));
    json += ",\"subsystem\":\"";
    json += runtimeDiagnosticSubsystemKey(
        static_cast<RuntimeDiagnosticSubsystem>(record->subsystem));
    json += "\",\"stage\":\"";
    json += runtimeDiagnosticStageKey(
        static_cast<RuntimeDiagnosticStage>(record->stage));
    json += "\",\"outcome\":\"";
    json += runtimeDiagnosticOutcomeKey(
        static_cast<RuntimeDiagnosticOutcome>(record->outcome));
    json += "\",\"detail0\":";
    json += String(static_cast<unsigned long>(record->detail0));
    json += ",\"detail1\":";
    json += String(static_cast<unsigned long>(record->detail1));
    json += ",\"internal_free\":";
    json += String(static_cast<unsigned long>(record->internalFree));
    json += ",\"internal_largest\":";
    json += String(static_cast<unsigned long>(record->internalLargest));
    json += ",\"psram_free\":";
    json += String(static_cast<unsigned long>(record->psramFree));
    json += ",\"reset_reason\":";
    json += String(static_cast<unsigned>(record->resetReason));
    json += '}';
  }
  json += "]";
  json += ",\"audio_session\":";
  if (!audioSnapshotAvailable) {
    json += "null";
  } else {
    json += "{\"generation\":";
    json += String(static_cast<unsigned long>(audioSnapshot.generation));
    json += ",\"session_id\":";
    json += String(static_cast<unsigned long>(audioSnapshot.sessionId));
    json += ",\"read_calls\":";
    json += String(static_cast<unsigned long>(audioSnapshot.readCalls));
    json += ",\"short_reads\":";
    json += String(static_cast<unsigned long>(audioSnapshot.shortReads));
    json += ",\"partial_mono_samples\":";
    json += String(static_cast<unsigned long>(
        audioSnapshot.partialMonoSamples));
    json += ",\"timeouts\":";
    json += String(static_cast<unsigned long>(audioSnapshot.i2sTimeouts));
    json += ",\"zero_reads\":";
    json += String(static_cast<unsigned long>(audioSnapshot.zeroByteReads));
    json += ",\"early_zero_reads\":";
    json += String(static_cast<unsigned long>(audioSnapshot.earlyZeroReads));
    json += ",\"longest_read_us\":";
    json += String(static_cast<unsigned long>(audioSnapshot.i2sLongestReadUs));
    json += ",\"ring_high_water_frames\":";
    json += String(static_cast<unsigned long>(
        audioSnapshot.captureRingHighWaterFrames));
    json += ",\"ring_drops\":";
    json += String(static_cast<unsigned long>(
        audioSnapshot.captureRingDroppedFrames));
    json += ",\"dispatch_consumed_frames\":";
    json += String(static_cast<unsigned long>(
        audioSnapshot.dispatchConsumedFrames));
    json += ",\"dispatch_sequence_gaps\":";
    json += String(static_cast<unsigned long>(
        audioSnapshot.dispatchSequenceGaps));
    json += ",\"dispatch_routing_failures\":";
    json += String(static_cast<unsigned long>(
        audioSnapshot.dispatchRoutingFailures));
    json += ",\"dispatch_recorder_delivery_failures\":";
    json += String(static_cast<unsigned long>(
        audioSnapshot.dispatchRecorderDeliveryFailures));
    json += ",\"dispatch_ble_delivery_failures\":";
    json += String(static_cast<unsigned long>(
        audioSnapshot.dispatchBleDeliveryFailures));
    json += ",\"first_failure\":\"";
    json += audioCaptureFailureCodeName(audioSnapshot.firstFailure);
    json += "\",\"first_failure_at_ms\":";
    json += String(static_cast<unsigned long>(audioSnapshot.firstFailureAtMs));
    json += ",\"first_failure_sequence\":";
    json += String(static_cast<unsigned long>(
        audioSnapshot.firstFailureSequence));
    json += ",\"capture_stack_high_water_words\":";
    json += String(static_cast<unsigned long>(
        audioSnapshot.captureTaskStackHighWaterWords));
    json += ",\"recorder_stack_high_water_words\":";
    json += String(static_cast<unsigned long>(
        audioSnapshot.recorderTaskStackHighWaterWords));
    json += ",\"source_overrun_observable\":";
    json += audioSnapshot.sourceOverrunObservable ? "true" : "false";
    json += ",\"frozen\":";
    json += audioSnapshot.frozen ? "true" : "false";
    json += '}';
  }
  json += "}";
  return json;
}

String RuntimeDiagnostics::traceJson(size_t newestOffset, size_t limit) const {
  if (limit > kRuntimeDiagnosticTracePageCapacity) {
    limit = kRuntimeDiagnosticTracePageCapacity;
  }
  RuntimeDiagnosticTraceRecord page[kRuntimeDiagnosticTracePageCapacity]{};
  size_t total = 0;
  size_t copied = 0;
  const_cast<RuntimeDiagnostics *>(this)->lock();
  total = trace_.count;
  while (copied < limit && newestOffset + copied < trace_.count) {
    const RuntimeDiagnosticTraceRecord *record =
        runtimeDiagnosticTraceNewest(trace_, newestOffset + copied);
    if (record == nullptr) break;
    page[copied++] = *record;
  }
  const_cast<RuntimeDiagnostics *>(this)->unlock();

  String json;
  json.reserve(3072);
  json = "{\"status\":\"ok\",\"version\":1,\"total\":";
  json += String(static_cast<unsigned>(total));
  json += ",\"offset\":";
  json += String(static_cast<unsigned>(newestOffset));
  json += ",\"count\":";
  json += String(static_cast<unsigned>(copied));
  json += ",\"next_offset\":";
  if (newestOffset + copied < total) {
    json += String(static_cast<unsigned>(newestOffset + copied));
  } else {
    json += "null";
  }
  json += ",\"records\":[";
  for (size_t index = 0; index < copied; ++index) {
    if (index != 0) json += ',';
    const RuntimeDiagnosticTraceRecord &traceRecord = page[index];
    const StoredRuntimeDiagnosticRecord &record = traceRecord.record;
    json += "{\"trace_sequence\":";
    json += String(static_cast<unsigned long>(traceRecord.traceSequence));
    json += ",\"epoch\":";
    json += String(static_cast<unsigned long>(record.epoch));
    json += ",\"uptime_ms\":";
    json += String(static_cast<unsigned long>(record.uptimeMs));
    json += ",\"subsystem\":\"";
    json += runtimeDiagnosticSubsystemKey(
        static_cast<RuntimeDiagnosticSubsystem>(record.subsystem));
    json += "\",\"stage\":\"";
    json += runtimeDiagnosticStageKey(
        static_cast<RuntimeDiagnosticStage>(record.stage));
    json += "\",\"outcome\":\"";
    json += runtimeDiagnosticOutcomeKey(
        static_cast<RuntimeDiagnosticOutcome>(record.outcome));
    json += "\",\"detail0\":";
    json += String(static_cast<unsigned long>(record.detail0));
    json += ",\"detail1\":";
    json += String(static_cast<unsigned long>(record.detail1));
    json += ",\"internal_free\":";
    json += String(static_cast<unsigned long>(record.internalFree));
    json += ",\"internal_largest\":";
    json += String(static_cast<unsigned long>(record.internalLargest));
    json += ",\"psram_free\":";
    json += String(static_cast<unsigned long>(record.psramFree));
    json += ",\"reset_reason\":";
    json += String(static_cast<unsigned>(record.resetReason));
    json += '}';
  }
  json += "]}";
  return json;
}

void RuntimeDiagnostics::publishAudioSessionSnapshot(
    const AudioSessionTelemetrySnapshot &snapshot) {
  lock();
  audioSessionSnapshot_ = snapshot;
  audioSessionSnapshotAvailable_ = true;
  unlock();
}

bool RuntimeDiagnostics::hasAudioSessionSnapshot() const {
  const_cast<RuntimeDiagnostics *>(this)->lock();
  const bool available = audioSessionSnapshotAvailable_;
  const_cast<RuntimeDiagnostics *>(this)->unlock();
  return available;
}

AudioSessionTelemetrySnapshot RuntimeDiagnostics::audioSessionSnapshot() const {
  const_cast<RuntimeDiagnostics *>(this)->lock();
  const AudioSessionTelemetrySnapshot snapshot = audioSessionSnapshot_;
  const_cast<RuntimeDiagnostics *>(this)->unlock();
  return snapshot;
}

void RuntimeDiagnostics::lock() {
#if defined(ARDUINO)
  if (mutex_ != nullptr) xSemaphoreTake(mutex_, portMAX_DELAY);
#else
  mutex_.lock();
#endif
}

void RuntimeDiagnostics::unlock() {
#if defined(ARDUINO)
  if (mutex_ != nullptr) xSemaphoreGive(mutex_);
#else
  mutex_.unlock();
#endif
}

}  // namespace pokepod
