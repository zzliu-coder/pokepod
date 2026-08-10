#include <cassert>
#include <cstring>

#include "PowerDiagnosticsCodec.h"
#include "PowerPolicy.h"

using namespace pokepod;

int main() {
  StoredPowerLog log{};
  initializePowerLog(log);
  finalizePowerLog(log);
  assert(validatePowerLog(log));
  assert(powerLogNewest(log, 0) == nullptr);

  StoredPowerLogRecord first{};
  first.event = static_cast<uint8_t>(PowerLogEvent::boot);
  first.wakeCause = 3;
  first.ext1WakeMask = 1ULL << 21;
  appendPowerLog(log, first);
  finalizePowerLog(log);
  assert(validatePowerLog(log));
  assert(powerLogNewest(log, 0)->sequence == 1);
  assert(powerLogNewest(log, 0)->ext1WakeMask == (1ULL << 21));

  for (size_t index = 0; index < kPowerLogCapacity + 3; ++index) {
    StoredPowerLogRecord item{};
    item.event = static_cast<uint8_t>(PowerLogEvent::sleepBlocked);
    item.blockers = static_cast<uint32_t>(index);
    appendPowerLog(log, item);
  }
  finalizePowerLog(log);
  assert(validatePowerLog(log));
  assert(log.count == kPowerLogCapacity);
  assert(powerLogNewest(log, 0)->blockers == kPowerLogCapacity + 2);

  StoredPowerLog corrupt = log;
  corrupt.records[0].blockers ^= 1;
  assert(!validatePowerLog(corrupt));
  corrupt = log;
  corrupt.version = 99;
  finalizePowerLog(corrupt);
  assert(!validatePowerLog(corrupt));
  corrupt = log;
  corrupt.count = kPowerLogCapacity + 1;
  finalizePowerLog(corrupt);
  assert(!validatePowerLog(corrupt));
  corrupt = log;
  corrupt.records[0].mode = 9;
  finalizePowerLog(corrupt);
  assert(!validatePowerLog(corrupt));

  assert(shouldPersistBlockedObservation(60000, 0, 1, 0, 4, 0, 30000));
  assert(!shouldPersistBlockedObservation(61000, 60000, 1, 1, 4, 4,
                                          30000));
  assert(!shouldPersistBlockedObservation(70000, 60000, 2, 1, 8, 4,
                                          30000));
  assert(shouldPersistBlockedObservation(90000, 60000, 2, 1, 8, 4,
                                         30000));
  assert(!shouldPersistBlockedObservation(90000, 60000, 2, 1, 0, 4,
                                          30000));
  assert(shouldPersistLightWake(1, 1000));
  assert(shouldPersistLightWake(16, 1000));
  assert(shouldPersistLightWake(2, 100));
  assert(!shouldPersistLightWake(2, 1000));
  assert(shouldPersistAutomaticScreenWake(1));
  assert(shouldPersistAutomaticScreenWake(8));
  assert(!shouldPersistAutomaticScreenWake(2));

  PowerInputs input;
  assert(lightSleepBlockers(input) &
         powerBlockerBit(PowerBlocker::screenOn));
  assert(lightSleepBlockers(input) &
         powerBlockerBit(PowerBlocker::beforeLightTimeout));
  input.screenOn = false;
  input.idleMs = kLightSleepTimeoutMs;
  assert(lightSleepBlockers(input) &
         powerBlockerBit(PowerBlocker::raiseToWake));
  input.automaticWakeEnabled = false;
  assert(lightSleepBlockers(input) == 0);
  input.idleMs = kDeepSleepTimeoutMs;
  input.vbusPresent = true;
  assert(deepSleepBlockers(input) &
         powerBlockerBit(PowerBlocker::vbusPresent));
  assert((deepSleepBlockers(input) &
          powerBlockerBit(PowerBlocker::raiseToWake)) == 0);
  input.vbusPresent = false;
  input.wifiRadioOn = true;
  input.bleConnected = true;
  assert(deepSleepBlockers(input) &
         powerBlockerBit(PowerBlocker::wifiRadio));
  assert(deepSleepBlockers(input) &
         powerBlockerBit(PowerBlocker::bleRadio));
  return 0;
}
