#pragma once

#include <mutex>

#if defined(ARDUINO)
#include <Arduino.h>
#include <Preferences.h>
#else
class Print;
class String;
#endif

#if defined(ARDUINO)
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#endif

#include "RuntimeDiagnosticsCodec.h"
#include "AudioSessionTelemetry.h"

namespace pokepod {

class RuntimeDiagnostics {
 public:
  bool begin(Print &log, uint16_t resetReason);
  bool record(RuntimeDiagnosticSubsystem subsystem,
              RuntimeDiagnosticStage stage,
              RuntimeDiagnosticOutcome outcome,
              uint32_t detail0, uint32_t detail1, Print &log);
  bool clear(Print &log);
  String json() const;

  // The terminal capture snapshot is intentionally RAM-only.  Publishing it
  // cannot perform an NVS write, so a storage/capture completion path remains
  // bounded even when diagnostics are enabled.  It stays available through
  // the status JSON until the next boot or an explicit clear.
  void publishAudioSessionSnapshot(
      const AudioSessionTelemetrySnapshot &snapshot);
  bool hasAudioSessionSnapshot() const;
  AudioSessionTelemetrySnapshot audioSessionSnapshot() const;

  size_t count() const { return stored_.count; }
  const StoredRuntimeDiagnosticRecord *newest(size_t offset) const {
    return runtimeDiagnosticNewest(stored_, offset);
  }

 private:
  bool persist(const StoredRuntimeDiagnosticLog &proposed);
  void lock();
  void unlock();

#if defined(ARDUINO)
  Preferences preferences_;
#endif
  StoredRuntimeDiagnosticLog stored_{};
  AudioSessionTelemetrySnapshot audioSessionSnapshot_{};
  bool audioSessionSnapshotAvailable_ = false;
  bool open_ = false;
#if defined(ARDUINO)
  SemaphoreHandle_t mutex_ = nullptr;
  StaticSemaphore_t mutexStorage_{};
#else
  mutable std::recursive_mutex mutex_;
#endif
};

}  // namespace pokepod
