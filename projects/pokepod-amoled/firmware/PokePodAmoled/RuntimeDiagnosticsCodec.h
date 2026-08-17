#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace pokepod {

constexpr uint32_t kRuntimeDiagnosticsMagic = 0x31524450;  // PDR1.
constexpr uint16_t kRuntimeDiagnosticsVersion = 1;
// Keep the complete JSON payload below the Link control-frame budget while
// retaining enough history to reconstruct the last failed operation.
constexpr size_t kRuntimeDiagnosticsCapacity = 12;
constexpr size_t kRuntimeDiagnosticTraceCapacity = 64;
constexpr size_t kRuntimeDiagnosticTracePageCapacity = 8;

enum class RuntimeDiagnosticSubsystem : uint8_t {
  boot = 1,
  recording = 2,
  provisioning = 3,
  wirelessVoice = 4,
  link = 5,
  radioControl = 6,
  audioControl = 7,
};

enum class RuntimeDiagnosticStage : uint8_t {
  boot = 1,
  recordingRequest = 2,
  recordingStorageReserve = 3,
  recordingCapacity = 4,
  recordingProbeOpen = 5,
  recordingProbeWrite = 6,
  recordingProbeFlush = 7,
  recordingProbeClose = 8,
  recordingProbeRemove = 9,
  recordingMetadata = 10,
  recordingAudioOpen = 11,
  recordingStarted = 12,
  recordingFailure = 13,
  recordingCleanup = 14,
  provisioningRequest = 20,
  provisioningQuiesceBefore = 21,
  provisioningQuiesceAfter = 22,
  provisioningModeBefore = 23,
  provisioningModeAfter = 24,
  provisioningSoftApBefore = 25,
  provisioningSoftApAfter = 26,
  provisioningServicesBefore = 27,
  provisioningServicesAfter = 28,
  provisioningStop = 29,
  wirelessAttempt = 40,
  wirelessRouterAcquire = 41,
  wirelessCaptureStart = 42,
  wirelessSessionStart = 43,
  wirelessReady = 44,
  wirelessFailure = 45,
  wirelessStop = 46,
  linkStallRecovery = 60,
  linkRecordingStartTimeout = 61,
  radioDisconnect = 70,
  radioModeOff = 71,
  radioScanCleanup = 72,
  audioI2sBegin = 80,
  audioCodecCreate = 81,
  audioCodecInit = 82,
};

enum class RuntimeDiagnosticOutcome : uint8_t {
  started = 0,
  success = 1,
  failure = 2,
  interrupted = 3,
};

#pragma pack(push, 1)
struct StoredRuntimeDiagnosticRecord {
  uint32_t sequence;
  uint32_t epoch;
  uint32_t uptimeMs;
  uint32_t detail0;
  uint32_t detail1;
  uint32_t internalFree;
  uint32_t internalLargest;
  uint32_t psramFree;
  uint16_t resetReason;
  uint8_t subsystem;
  uint8_t stage;
  uint8_t outcome;
  uint8_t reserved;
};

struct StoredRuntimeDiagnosticLog {
  uint32_t magic;
  uint16_t version;
  uint8_t count;
  uint8_t next;
  uint32_t nextSequence;
  StoredRuntimeDiagnosticRecord records[kRuntimeDiagnosticsCapacity];
  uint32_t crc32;
};

struct RuntimeDiagnosticTraceRecord {
  uint32_t traceSequence;
  StoredRuntimeDiagnosticRecord record;
};

struct RuntimeDiagnosticTraceLog {
  uint16_t count;
  uint16_t next;
  uint32_t nextSequence;
  RuntimeDiagnosticTraceRecord records[kRuntimeDiagnosticTraceCapacity];
};
#pragma pack(pop)

static_assert(sizeof(StoredRuntimeDiagnosticRecord) == 38,
              "runtime diagnostic record layout is persistent");
static_assert(sizeof(StoredRuntimeDiagnosticLog) < 1024,
              "runtime diagnostic log must remain a bounded NVS blob");
static_assert(sizeof(RuntimeDiagnosticTraceLog) < 4096,
              "runtime diagnostic trace must remain a bounded RAM object");

inline uint32_t runtimeDiagnosticsCrc32(const uint8_t *data, size_t length) {
  uint32_t crc = 0xffffffffU;
  for (size_t index = 0; index < length; ++index) {
    crc ^= data[index];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1U) ^ (0xedb88320U &
          static_cast<uint32_t>(-static_cast<int32_t>(crc & 1U)));
    }
  }
  return ~crc;
}

inline void initializeRuntimeDiagnosticLog(StoredRuntimeDiagnosticLog &log) {
  std::memset(&log, 0, sizeof(log));
  log.magic = kRuntimeDiagnosticsMagic;
  log.version = kRuntimeDiagnosticsVersion;
  log.nextSequence = 1;
}

inline void finalizeRuntimeDiagnosticLog(StoredRuntimeDiagnosticLog &log) {
  log.crc32 = runtimeDiagnosticsCrc32(
      reinterpret_cast<const uint8_t *>(&log),
      offsetof(StoredRuntimeDiagnosticLog, crc32));
}

inline bool validateRuntimeDiagnosticLog(
    const StoredRuntimeDiagnosticLog &log) {
  if (log.magic != kRuntimeDiagnosticsMagic ||
      log.version != kRuntimeDiagnosticsVersion ||
      log.count > kRuntimeDiagnosticsCapacity ||
      log.next >= kRuntimeDiagnosticsCapacity || log.nextSequence == 0 ||
      log.crc32 != runtimeDiagnosticsCrc32(
          reinterpret_cast<const uint8_t *>(&log),
          offsetof(StoredRuntimeDiagnosticLog, crc32))) {
    return false;
  }
  for (size_t index = 0; index < kRuntimeDiagnosticsCapacity; ++index) {
    const StoredRuntimeDiagnosticRecord &record = log.records[index];
    if (record.subsystem > static_cast<uint8_t>(
            RuntimeDiagnosticSubsystem::audioControl) ||
        record.outcome > static_cast<uint8_t>(
            RuntimeDiagnosticOutcome::interrupted)) {
      return false;
    }
  }
  return true;
}

inline void appendRuntimeDiagnostic(
    StoredRuntimeDiagnosticLog &log, StoredRuntimeDiagnosticRecord record) {
  record.sequence = log.nextSequence++;
  if (log.nextSequence == 0) log.nextSequence = 1;
  log.records[log.next] = record;
  log.next = static_cast<uint8_t>(
      (log.next + 1U) % kRuntimeDiagnosticsCapacity);
  if (log.count < kRuntimeDiagnosticsCapacity) ++log.count;
}

inline const StoredRuntimeDiagnosticRecord *runtimeDiagnosticNewest(
    const StoredRuntimeDiagnosticLog &log, size_t newestOffset) {
  if (newestOffset >= log.count) return nullptr;
  const size_t newest =
      (log.next + kRuntimeDiagnosticsCapacity - 1U) %
      kRuntimeDiagnosticsCapacity;
  const size_t index =
      (newest + kRuntimeDiagnosticsCapacity - newestOffset) %
      kRuntimeDiagnosticsCapacity;
  return &log.records[index];
}

inline void initializeRuntimeDiagnosticTrace(RuntimeDiagnosticTraceLog &log) {
  std::memset(&log, 0, sizeof(log));
  log.nextSequence = 1;
}

inline void appendRuntimeDiagnosticTrace(
    RuntimeDiagnosticTraceLog &log, StoredRuntimeDiagnosticRecord record) {
  RuntimeDiagnosticTraceRecord &slot = log.records[log.next];
  slot.traceSequence = log.nextSequence++;
  if (log.nextSequence == 0) log.nextSequence = 1;
  slot.record = record;
  log.next = static_cast<uint16_t>(
      (log.next + 1U) % kRuntimeDiagnosticTraceCapacity);
  if (log.count < kRuntimeDiagnosticTraceCapacity) ++log.count;
}

inline const RuntimeDiagnosticTraceRecord *runtimeDiagnosticTraceNewest(
    const RuntimeDiagnosticTraceLog &log, size_t newestOffset) {
  if (newestOffset >= log.count) return nullptr;
  const size_t newest =
      (log.next + kRuntimeDiagnosticTraceCapacity - 1U) %
      kRuntimeDiagnosticTraceCapacity;
  const size_t index =
      (newest + kRuntimeDiagnosticTraceCapacity - newestOffset) %
      kRuntimeDiagnosticTraceCapacity;
  return &log.records[index];
}

inline bool runtimeDiagnosticShouldPersist(
    RuntimeDiagnosticSubsystem subsystem, RuntimeDiagnosticStage stage,
    RuntimeDiagnosticOutcome outcome) {
  if (outcome == RuntimeDiagnosticOutcome::failure ||
      outcome == RuntimeDiagnosticOutcome::interrupted) {
    return true;
  }
  if (subsystem == RuntimeDiagnosticSubsystem::boot &&
      stage == RuntimeDiagnosticStage::boot) {
    return true;
  }
  if (subsystem == RuntimeDiagnosticSubsystem::recording &&
      stage == RuntimeDiagnosticStage::recordingCleanup) {
    return true;
  }
  if (subsystem == RuntimeDiagnosticSubsystem::provisioning &&
      (stage == RuntimeDiagnosticStage::provisioningServicesAfter ||
       stage == RuntimeDiagnosticStage::provisioningStop)) {
    return true;
  }
  if (subsystem == RuntimeDiagnosticSubsystem::wirelessVoice &&
      stage == RuntimeDiagnosticStage::wirelessStop) {
    return true;
  }
  // These calls cross into radio, DMA and codec drivers. Persist both sides
  // of the boundary so a watchdog/manual reset can distinguish "never
  // entered", "did not return" and "returned failure" without a live Link.
  if (subsystem == RuntimeDiagnosticSubsystem::radioControl ||
      subsystem == RuntimeDiagnosticSubsystem::audioControl) {
    return true;
  }
  if (subsystem == RuntimeDiagnosticSubsystem::link &&
      stage == RuntimeDiagnosticStage::linkRecordingStartTimeout) {
    return true;
  }
  return false;
}

inline const char *runtimeDiagnosticSubsystemKey(
    RuntimeDiagnosticSubsystem subsystem) {
  switch (subsystem) {
    case RuntimeDiagnosticSubsystem::boot: return "boot";
    case RuntimeDiagnosticSubsystem::recording: return "recording";
    case RuntimeDiagnosticSubsystem::provisioning: return "provisioning";
    case RuntimeDiagnosticSubsystem::wirelessVoice: return "wireless_voice";
    case RuntimeDiagnosticSubsystem::link: return "link";
    case RuntimeDiagnosticSubsystem::radioControl: return "radio_control";
    case RuntimeDiagnosticSubsystem::audioControl: return "audio_control";
  }
  return "unknown";
}

inline const char *runtimeDiagnosticStageKey(RuntimeDiagnosticStage stage) {
  switch (stage) {
    case RuntimeDiagnosticStage::boot: return "boot";
    case RuntimeDiagnosticStage::recordingRequest: return "recording_request";
    case RuntimeDiagnosticStage::recordingStorageReserve:
      return "recording_storage_reserve";
    case RuntimeDiagnosticStage::recordingCapacity:
      return "recording_capacity";
    case RuntimeDiagnosticStage::recordingProbeOpen:
      return "recording_probe_open";
    case RuntimeDiagnosticStage::recordingProbeWrite:
      return "recording_probe_write";
    case RuntimeDiagnosticStage::recordingProbeFlush:
      return "recording_probe_flush";
    case RuntimeDiagnosticStage::recordingProbeClose:
      return "recording_probe_close";
    case RuntimeDiagnosticStage::recordingProbeRemove:
      return "recording_probe_remove";
    case RuntimeDiagnosticStage::recordingMetadata:
      return "recording_metadata";
    case RuntimeDiagnosticStage::recordingAudioOpen:
      return "recording_audio_open";
    case RuntimeDiagnosticStage::recordingStarted:
      return "recording_started";
    case RuntimeDiagnosticStage::recordingFailure:
      return "recording_failure";
    case RuntimeDiagnosticStage::recordingCleanup:
      return "recording_cleanup";
    case RuntimeDiagnosticStage::provisioningRequest:
      return "provisioning_request";
    case RuntimeDiagnosticStage::provisioningQuiesceBefore:
      return "provisioning_quiesce_before";
    case RuntimeDiagnosticStage::provisioningQuiesceAfter:
      return "provisioning_quiesce_after";
    case RuntimeDiagnosticStage::provisioningModeBefore:
      return "provisioning_mode_before";
    case RuntimeDiagnosticStage::provisioningModeAfter:
      return "provisioning_mode_after";
    case RuntimeDiagnosticStage::provisioningSoftApBefore:
      return "provisioning_softap_before";
    case RuntimeDiagnosticStage::provisioningSoftApAfter:
      return "provisioning_softap_after";
    case RuntimeDiagnosticStage::provisioningServicesBefore:
      return "provisioning_services_before";
    case RuntimeDiagnosticStage::provisioningServicesAfter:
      return "provisioning_services_after";
    case RuntimeDiagnosticStage::provisioningStop:
      return "provisioning_stop";
    case RuntimeDiagnosticStage::wirelessAttempt:
      return "wireless_attempt";
    case RuntimeDiagnosticStage::wirelessRouterAcquire:
      return "wireless_router_acquire";
    case RuntimeDiagnosticStage::wirelessCaptureStart:
      return "wireless_capture_start";
    case RuntimeDiagnosticStage::wirelessSessionStart:
      return "wireless_session_start";
    case RuntimeDiagnosticStage::wirelessReady:
      return "wireless_ready";
    case RuntimeDiagnosticStage::wirelessFailure:
      return "wireless_failure";
    case RuntimeDiagnosticStage::wirelessStop:
      return "wireless_stop";
    case RuntimeDiagnosticStage::linkStallRecovery:
      return "link_stall_recovery";
    case RuntimeDiagnosticStage::linkRecordingStartTimeout:
      return "link_recording_start_timeout";
    case RuntimeDiagnosticStage::radioDisconnect:
      return "radio_disconnect";
    case RuntimeDiagnosticStage::radioModeOff:
      return "radio_mode_off";
    case RuntimeDiagnosticStage::radioScanCleanup:
      return "radio_scan_cleanup";
    case RuntimeDiagnosticStage::audioI2sBegin:
      return "audio_i2s_begin";
    case RuntimeDiagnosticStage::audioCodecCreate:
      return "audio_codec_create";
    case RuntimeDiagnosticStage::audioCodecInit:
      return "audio_codec_init";
  }
  return "unknown";
}

inline const char *runtimeDiagnosticOutcomeKey(
    RuntimeDiagnosticOutcome outcome) {
  switch (outcome) {
    case RuntimeDiagnosticOutcome::started: return "started";
    case RuntimeDiagnosticOutcome::success: return "success";
    case RuntimeDiagnosticOutcome::failure: return "failure";
    case RuntimeDiagnosticOutcome::interrupted: return "interrupted";
  }
  return "unknown";
}

}  // namespace pokepod
