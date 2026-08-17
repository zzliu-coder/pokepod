#pragma once

#include <Arduino.h>
#include <Preferences.h>

#include "ProvisioningLogCodec.h"
#include "ProvisioningProbeCodec.h"
#include "ProvisioningPersistencePolicy.h"

namespace pokepod {

class ProvisioningDiagnostics {
 public:
  bool begin(Print &log, uint16_t resetReason = 0);
  bool record(ProvisioningLogStage stage, ProvisioningLogOutcome outcome,
              const String &ssid, int16_t rssi, uint16_t reason,
              uint32_t elapsedMs, uint8_t attempt, Print &log);
  bool clear(Print &log);
  bool recordProbe(ProvisioningProbeStage stage, Print &log);
  size_t count() const { return stored_.count; }
  bool recoveredInterruptedSession() const {
    return recoveredInterruptedSession_;
  }
  uint32_t revision() const { return revision_; }
  uint16_t persistentWrites() const { return persistentWrites_; }
  void recordPollDuration(uint32_t elapsedMs, Print &log);
  uint32_t pollCount() const { return pollCount_; }
  uint32_t pollMaxMs() const { return pollMaxMs_; }
  const StoredProvisioningProbe &probe() const { return probe_; }
  const StoredProvisioningLogRecord *newest(size_t offset) const {
    return provisioningLogNewest(stored_, offset);
  }

 private:
  bool persist(const StoredProvisioningLog &proposed);
  bool persistProbe(const StoredProvisioningProbe &proposed);
  void printProbeBoot(Print &log, uint16_t currentResetReason,
                      bool recovered) const;

  Preferences preferences_;
  StoredProvisioningLog stored_{};
  StoredProvisioningProbe probe_{};
  bool open_ = false;
  bool recoveredInterruptedSession_ = false;
  uint32_t revision_ = 0;
  uint16_t persistentWrites_ = 0;
  uint32_t pollCount_ = 0;
  uint32_t pollMaxMs_ = 0;
  uint16_t pollBuckets_[8] = {};
};

class ProvisioningPollScope final {
 public:
  ProvisioningPollScope(ProvisioningDiagnostics *diagnostics, Print *log)
      : diagnostics_(diagnostics), log_(log), startedMs_(millis()) {}
  ~ProvisioningPollScope() {
    if (diagnostics_ != nullptr && log_ != nullptr) {
      diagnostics_->recordPollDuration(millis() - startedMs_, *log_);
    }
  }

 private:
  ProvisioningDiagnostics *diagnostics_ = nullptr;
  Print *log_ = nullptr;
  uint32_t startedMs_ = 0;
};

const char *provisioningLogStageKey(ProvisioningLogStage stage);
String provisioningLogStageLabel(ProvisioningLogStage stage);
String provisioningLogReasonLabel(const StoredProvisioningLogRecord &record);

}  // namespace pokepod
