#pragma once

#include <Arduino.h>
#include <Preferences.h>

#include "PowerDiagnosticsCodec.h"
#include "PowerPolicy.h"

namespace pokepod {

enum class AutomaticScreenWakeSource : uint8_t {
  touchInterrupt = 1,
  motionInterrupt = 2,
  raiseToWakePolicy = 3,
};

struct PowerDiagnosticsSnapshot {
  PowerBlockerMask activeFacts = 0;
  PowerBlockerMask lightBlockers = 0;
  PowerBlockerMask deepBlockers = 0;
  PowerBlockerMask currentBlockers = 0;
  uint32_t idleMs = 0;
  uint32_t revision = 0;
  uint32_t persistFailures = 0;
  uint32_t automaticScreenWakes = 0;
  int8_t batteryPercent = -1;
};

class PowerDiagnostics {
 public:
  bool begin(Print &log, uint16_t resetReason, uint8_t wakeCause,
             uint32_t wakeCauses, uint64_t ext1WakeMask,
             bool automaticPmSupported, bool bleModemSleepSupported,
             int batteryPercent);
  void observe(uint32_t nowMs, const PowerInputs &inputs,
               const PowerDecision &decision, int batteryPercent,
               Print &log);
  void recordLightSleepWake(uint32_t nowMs, uint32_t durationMs,
                            uint8_t wakeCause, int batteryPercent,
                            Print &log);
  void recordLightSleepError(uint32_t nowMs, int32_t error,
                             const PowerInputs &inputs, int batteryPercent,
                             Print &log);
  void recordDeepSleepIntent(uint32_t nowMs, const PowerInputs &inputs,
                             uint64_t armedWakeMask, int batteryPercent,
                             Print &log);
  void recordDeepSleepArmError(uint32_t nowMs, int32_t error,
                               const PowerInputs &inputs, int batteryPercent,
                               Print &log);
  void recordSafeShutdown(uint32_t nowMs, const PowerInputs &inputs,
                          int batteryPercent, Print &log);
  void recordAutomaticScreenWake(uint32_t nowMs,
                                 AutomaticScreenWakeSource source,
                                 const PowerInputs &inputs,
                                 int batteryPercent, Print &log);
  bool clear(Print &log);

  size_t count() const { return stored_.count; }
  const StoredPowerLogRecord *newest(size_t offset) const {
    return powerLogNewest(stored_, offset);
  }
  const PowerDiagnosticsSnapshot &snapshot() const { return snapshot_; }

 private:
  bool persist(const StoredPowerLog &proposed);
  bool append(PowerLogEvent event, uint32_t nowMs, PowerMode mode,
              PowerBlockerMask blockers, uint32_t durationMs,
              uint32_t detail, uint64_t ext1WakeMask, int32_t error,
              uint16_t resetReason, uint8_t wakeCause, int batteryPercent,
              uint16_t flags, Print &log);
  uint16_t flagsFor(const PowerInputs *inputs) const;

  Preferences preferences_;
  StoredPowerLog stored_{};
  PowerDiagnosticsSnapshot snapshot_;
  bool open_ = false;
  bool automaticPmSupported_ = false;
  bool bleModemSleepSupported_ = false;
  uint8_t lastBlockedStage_ = 0;
  PowerBlockerMask lastPersistedBlockers_ = 0;
  uint32_t lastBlockedPersistedAtMs_ = 0;
  uint32_t lightWakeCountThisBoot_ = 0;
  uint32_t automaticScreenWakeCountThisBoot_ = 0;
  int32_t lastLightError_ = 0;
  uint32_t lastLightErrorAtMs_ = 0;
};

const char *powerLogEventKey(PowerLogEvent event);

}  // namespace pokepod
