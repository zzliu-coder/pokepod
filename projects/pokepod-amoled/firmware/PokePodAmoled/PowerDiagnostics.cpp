#include "PowerDiagnostics.h"

#include <algorithm>
#include <time.h>

namespace pokepod {
namespace {

constexpr char kPowerLogKey[] = "power_log_v1";
constexpr uint32_t kBlockedLogRateLimitMs = 30000;
constexpr uint32_t kLightErrorRateLimitMs = 30000;
constexpr uint32_t kValidEpoch = 1704067200;

int8_t boundedBattery(int value) {
  return value < 0 ? -1 : static_cast<int8_t>(std::min(value, 100));
}

bool elapsedAtLeast(uint32_t nowMs, uint32_t thenMs, uint32_t intervalMs) {
  return static_cast<uint32_t>(nowMs - thenMs) >= intervalMs;
}

}  // namespace

bool PowerDiagnostics::begin(
    Print &log, uint16_t resetReason, uint8_t wakeCause,
    uint32_t wakeCauses, uint64_t ext1WakeMask,
    bool automaticPmSupported, bool bleModemSleepSupported,
    int batteryPercent) {
  automaticPmSupported_ = automaticPmSupported;
  bleModemSleepSupported_ = bleModemSleepSupported;
  open_ = preferences_.begin("pokepod_diag", false);
  initializePowerLog(stored_);
  if (!open_) {
    log.println(
        "{\"event\":\"power_log\",\"ok\":false,\"stage\":\"nvs_open\"}");
    return false;
  }
  StoredPowerLog loaded{};
  const bool loadedOk =
      preferences_.getBytesLength(kPowerLogKey) == sizeof(loaded) &&
      preferences_.getBytes(kPowerLogKey, &loaded, sizeof(loaded)) ==
          sizeof(loaded) && validatePowerLog(loaded);
  if (loadedOk) stored_ = loaded;
  const bool recorded = append(
      PowerLogEvent::boot, millis(), PowerMode::balanced, 0, 0,
      wakeCauses, ext1WakeMask, 0, resetReason, wakeCause, batteryPercent,
      flagsFor(nullptr), log);
  log.printf(
      "{\"event\":\"power_log\",\"ok\":%s,\"count\":%u,\"recovered\":%s}\n",
      recorded ? "true" : "false", static_cast<unsigned>(stored_.count),
      loadedOk ? "true" : "false");
  return recorded;
}

bool PowerDiagnostics::persist(const StoredPowerLog &proposed) {
  return open_ && preferences_.putBytes(
      kPowerLogKey, &proposed, sizeof(proposed)) == sizeof(proposed);
}

uint16_t PowerDiagnostics::flagsFor(const PowerInputs *inputs) const {
  uint16_t flags = 0;
  if (automaticPmSupported_) flags |= kPowerFlagAutomaticPm;
  if (bleModemSleepSupported_) flags |= kPowerFlagBleModemSleep;
  if (inputs == nullptr) return flags;
  if (inputs->screenOn) flags |= kPowerFlagScreenOn;
  if (inputs->automaticWakeEnabled) flags |= kPowerFlagAutomaticWake;
  if (inputs->usbHostConnected) flags |= kPowerFlagUsbHost;
  if (inputs->vbusPresent) flags |= kPowerFlagVbus;
  return flags;
}

bool PowerDiagnostics::append(
    PowerLogEvent event, uint32_t nowMs, PowerMode mode,
    PowerBlockerMask blockers, uint32_t durationMs, uint32_t detail,
    uint64_t ext1WakeMask, int32_t error, uint16_t resetReason,
    uint8_t wakeCause, int batteryPercent, uint16_t flags, Print &log) {
  if (!open_) return false;
  StoredPowerLog proposed = stored_;
  StoredPowerLogRecord record{};
  const time_t now = time(nullptr);
  record.epoch = now >= kValidEpoch ? static_cast<uint32_t>(now) : 0;
  record.uptimeMs = nowMs;
  record.durationMs = durationMs;
  record.blockers = blockers;
  record.detail = detail;
  record.ext1WakeMask = ext1WakeMask;
  record.error = error;
  record.flags = flags;
  record.resetReason = resetReason;
  record.event = static_cast<uint8_t>(event);
  record.mode = static_cast<uint8_t>(mode);
  record.wakeCause = wakeCause;
  record.batteryPercent = boundedBattery(batteryPercent);
  appendPowerLog(proposed, record);
  finalizePowerLog(proposed);
  const bool ok = persist(proposed);
  if (ok) {
    stored_ = proposed;
    ++snapshot_.revision;
  } else {
    ++snapshot_.persistFailures;
  }
  log.printf(
      "{\"event\":\"power_diagnostic\",\"ok\":%s,\"kind\":\"%s\",\"blockers\":%lu,\"duration_ms\":%lu,\"error\":%ld}\n",
      ok ? "true" : "false", powerLogEventKey(event),
      static_cast<unsigned long>(blockers),
      static_cast<unsigned long>(durationMs), static_cast<long>(error));
  return ok;
}

void PowerDiagnostics::observe(uint32_t nowMs, const PowerInputs &inputs,
                               const PowerDecision &decision,
                               int batteryPercent, Print &log) {
  snapshot_.activeFacts = activePowerFacts(inputs);
  snapshot_.lightBlockers = lightSleepBlockers(inputs);
  snapshot_.deepBlockers = deepSleepBlockers(inputs);
  snapshot_.currentBlockers = currentSleepBlockers(inputs);
  snapshot_.idleMs = inputs.idleMs;
  snapshot_.batteryPercent = boundedBattery(batteryPercent);

  if (inputs.screenOn || inputs.idleMs < kLightSleepTimeoutMs) {
    lastBlockedStage_ = 0;
    lastPersistedBlockers_ = 0;
    return;
  }
  const uint8_t stage = inputs.idleMs >= kDeepSleepTimeoutMs ? 2 : 1;
  const PowerBlockerMask blockers = snapshot_.currentBlockers;
  if (blockers == 0) {
    lastBlockedStage_ = stage;
    lastPersistedBlockers_ = 0;
    return;
  }
  if (!shouldPersistBlockedObservation(
          nowMs, lastBlockedPersistedAtMs_, stage, lastBlockedStage_,
          blockers, lastPersistedBlockers_, kBlockedLogRateLimitMs)) {
    return;
  }
  if (append(PowerLogEvent::sleepBlocked, nowMs, decision.mode, blockers,
             inputs.idleMs, stage, 0, 0, 0, 0, batteryPercent,
             flagsFor(&inputs), log)) {
    lastBlockedStage_ = stage;
    lastPersistedBlockers_ = blockers;
    lastBlockedPersistedAtMs_ = nowMs == 0 ? 1 : nowMs;
  }
}

void PowerDiagnostics::recordLightSleepWake(
    uint32_t nowMs, uint32_t durationMs, uint8_t wakeCause,
    int batteryPercent, Print &log) {
  ++lightWakeCountThisBoot_;
  if (!shouldPersistLightWake(lightWakeCountThisBoot_, durationMs)) {
    return;
  }
  (void)append(PowerLogEvent::lightSleepWake, nowMs,
               PowerMode::lightSleep, snapshot_.currentBlockers, durationMs,
               lightWakeCountThisBoot_, 0, 0, 0, wakeCause, batteryPercent,
               flagsFor(nullptr), log);
}

void PowerDiagnostics::recordLightSleepError(
    uint32_t nowMs, int32_t error, const PowerInputs &inputs,
    int batteryPercent, Print &log) {
  if (error == lastLightError_ && lastLightErrorAtMs_ != 0 &&
      !elapsedAtLeast(nowMs, lastLightErrorAtMs_, kLightErrorRateLimitMs)) {
    return;
  }
  if (append(PowerLogEvent::lightSleepError, nowMs, PowerMode::lightSleep,
             lightSleepBlockers(inputs), 0, 0, 0, error, 0, 0,
             batteryPercent, flagsFor(&inputs), log)) {
    lastLightError_ = error;
    lastLightErrorAtMs_ = nowMs == 0 ? 1 : nowMs;
  }
}

void PowerDiagnostics::recordDeepSleepIntent(
    uint32_t nowMs, const PowerInputs &inputs, uint64_t armedWakeMask,
    int batteryPercent, Print &log) {
  (void)append(PowerLogEvent::deepSleepIntent, nowMs,
               PowerMode::deepSleepPending, deepSleepBlockers(inputs),
               inputs.idleMs, 0, armedWakeMask, 0, 0, 0, batteryPercent,
               flagsFor(&inputs), log);
}

void PowerDiagnostics::recordDeepSleepArmError(
    uint32_t nowMs, int32_t error, const PowerInputs &inputs,
    int batteryPercent, Print &log) {
  (void)append(PowerLogEvent::deepSleepArmError, nowMs,
               PowerMode::deepSleepPending, deepSleepBlockers(inputs),
               inputs.idleMs, 0, 0, error, 0, 0, batteryPercent,
               flagsFor(&inputs), log);
}

void PowerDiagnostics::recordSafeShutdown(
    uint32_t nowMs, const PowerInputs &inputs, int batteryPercent,
    Print &log) {
  (void)append(PowerLogEvent::safeShutdown, nowMs,
               PowerMode::safeShutdownPending, activePowerFacts(inputs),
               inputs.idleMs, 0, 0, 0, 0, 0, batteryPercent,
               flagsFor(&inputs), log);
}

void PowerDiagnostics::recordAutomaticScreenWake(
    uint32_t nowMs, AutomaticScreenWakeSource source,
    const PowerInputs &inputs, int batteryPercent, Print &log) {
  ++automaticScreenWakeCountThisBoot_;
  snapshot_.automaticScreenWakes = automaticScreenWakeCountThisBoot_;
  if (!shouldPersistAutomaticScreenWake(
          automaticScreenWakeCountThisBoot_)) {
    return;
  }
  (void)append(PowerLogEvent::automaticScreenWake, nowMs,
               PowerMode::balanced, activePowerFacts(inputs), inputs.idleMs,
               static_cast<uint32_t>(source), 0, 0, 0, 0,
               batteryPercent, flagsFor(&inputs), log);
}

bool PowerDiagnostics::clear(Print &log) {
  StoredPowerLog proposed{};
  initializePowerLog(proposed);
  finalizePowerLog(proposed);
  const bool ok = persist(proposed);
  if (ok) {
    stored_ = proposed;
    ++snapshot_.revision;
    lastBlockedStage_ = 0;
    lastPersistedBlockers_ = 0;
    lastBlockedPersistedAtMs_ = 0;
    lastLightError_ = 0;
    lastLightErrorAtMs_ = 0;
  } else {
    ++snapshot_.persistFailures;
  }
  log.printf("{\"event\":\"power_log_cleared\",\"ok\":%s}\n",
             ok ? "true" : "false");
  return ok;
}

const char *powerLogEventKey(PowerLogEvent event) {
  switch (event) {
    case PowerLogEvent::boot: return "boot";
    case PowerLogEvent::sleepBlocked: return "sleep_blocked";
    case PowerLogEvent::lightSleepWake: return "light_sleep_wake";
    case PowerLogEvent::lightSleepError: return "light_sleep_error";
    case PowerLogEvent::deepSleepIntent: return "deep_sleep_intent";
    case PowerLogEvent::deepSleepArmError: return "deep_sleep_arm_error";
    case PowerLogEvent::safeShutdown: return "safe_shutdown";
    case PowerLogEvent::automaticScreenWake:
      return "automatic_screen_wake";
  }
  return "unknown";
}

}  // namespace pokepod
