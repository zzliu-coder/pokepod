#include <cassert>
#include <cstdint>

#include "HardwareSafetyPolicy.h"
#include "PowerFacts.h"
#include "PowerPolicy.h"
#include "UsbCdcSessionState.h"
#include "UsbPhysicalConnectionPolicy.h"

using namespace pokepod;

namespace {

PowerInputs idleInput(uint32_t idleMs) {
  PowerInputs input;
  input.screenOn = false;
  input.automaticWakeEnabled = false;
  input.idleMs = idleMs;
  return input;
}

bool hasBlocker(const PowerInputs &input, PowerBlocker blocker) {
  return (activePowerFacts(input) & powerBlockerBit(blocker)) != 0;
}

}  // namespace

int main() {
  // Every combination keeps physical mount, DTR, VBUS, charging and the Link
  // lease independent. A stale mount is deliberately absent from blockers.
  for (uint32_t combination = 0; combination < 32; ++combination) {
    PowerFacts facts;
    facts.usbMounted = (combination & 1U) != 0;
    facts.cdcSessionActive = (combination & 2U) != 0;
    facts.vbusPresent = (combination & 4U) != 0;
    facts.charging = (combination & 8U) != 0;
    facts.linkLeaseActive = (combination & 16U) != 0;

    PowerInputs input = powerInputsWithFacts(
        idleInput(kLightSleepTimeoutMs), facts);
    assert(input.usbMounted == facts.usbMounted);
    assert(input.cdcSessionActive == facts.cdcSessionActive);
    assert(hasBlocker(input, PowerBlocker::usbHost) ==
           facts.cdcSessionActive);
    assert(hasBlocker(input, PowerBlocker::vbusPresent) ==
           facts.vbusPresent);
    assert(hasBlocker(input, PowerBlocker::charging) == facts.charging);
    assert(hasBlocker(input, PowerBlocker::linkBusy) ==
           facts.linkLeaseActive);

    const PowerDecision light = decidePower(input);
    const bool foreground = facts.linkLeaseActive;
    const bool lightAllowed = !facts.cdcSessionActive && !foreground;
    assert(light.allowLightSleep == lightAllowed);

    input.idleMs = kDeepSleepTimeoutMs;
    const PowerDecision deep = decidePower(input);
    const bool deepAllowed = !facts.cdcSessionActive &&
        !facts.vbusPresent && !facts.charging && !facts.linkLeaseActive;
    assert(deep.requestDeepSleep == deepAllowed);
    if ((facts.vbusPresent || facts.charging) &&
        !facts.cdcSessionActive && !facts.linkLeaseActive) {
      assert(deep.allowLightSleep);
      assert(deep.lightSleepTimerUs ==
             static_cast<uint64_t>(kChargingLightSleepCheckMs) * 1000ULL);
    }
  }

  // A mounted-only ghost is idle-safe at both deadlines.
  PowerFacts mountedOnly;
  mountedOnly.usbMounted = true;
  PowerInputs input = powerInputsWithFacts(
      idleInput(kLightSleepTimeoutMs), mountedOnly);
  assert(lightSleepBlockers(input) == 0);
  assert(decidePower(input).allowLightSleep);
  input.idleMs = kDeepSleepTimeoutMs;
  assert(deepSleepBlockers(input) == 0);
  assert(decidePower(input).requestDeepSleep);

  // The absolute 60 s / 180 s boundaries do not depend on periodic UI work.
  input = idleInput(kLightSleepTimeoutMs - 1);
  assert(!decidePower(input).allowLightSleep);
  input.idleMs = kLightSleepTimeoutMs;
  assert(decidePower(input).allowLightSleep);
  input.idleMs = kDeepSleepTimeoutMs - 1;
  assert(!decidePower(input).requestDeepSleep);
  input.idleMs = kDeepSleepTimeoutMs;
  assert(decidePower(input).requestDeepSleep);

  // Enabling automatic wake only blocks light sleep until a real low-power
  // wake source is ready. Turning the feature off never consumes IMU events.
  PowerFacts wakeFacts;
  input = idleInput(kLightSleepTimeoutMs);
  input.automaticWakeEnabled = true;
  input = powerInputsWithFacts(input, wakeFacts);
  assert(automaticWakeUnavailable(input));
  assert(!decidePower(input).allowLightSleep);
  assert(!shouldArmAutomaticWake(true, wakeFacts));
  wakeFacts.wakeSourcesReady = true;
  input = powerInputsWithFacts(input, wakeFacts);
  assert(!automaticWakeUnavailable(input));
  assert(decidePower(input).allowLightSleep);
  assert(shouldArmAutomaticWake(true, wakeFacts));
  assert(!shouldArmAutomaticWake(false, wakeFacts));
  input.automaticWakeEnabled = false;
  wakeFacts.wakeSourcesReady = false;
  input = powerInputsWithFacts(input, wakeFacts);
  assert(decidePower(input).allowLightSleep);

  AutoScreenOffPolicy screen;
  screen.begin(1000);
  assert(!screen.shouldTurnOff(30999, true, false));
  assert(screen.shouldTurnOff(31000, true, false));

  // Hardware bad paths are explicit and testable without touching the board.
  assert(!canPollMotionWake(false, true, true));
  assert(!canPollMotionWake(true, false, true));
  assert(!canPollMotionWake(true, true, false));
  assert(canPollMotionWake(true, true, true));
  assert(!systemClockUpdateSucceeded(kMinimumTrustedUtcEpoch - 1, 0));
  assert(!systemClockUpdateSucceeded(kMinimumTrustedUtcEpoch, -1));
  assert(systemClockUpdateSucceeded(kMinimumTrustedUtcEpoch, 0));
  assert(shutdownFallbackAction(true, false, true) ==
         ShutdownFallbackAction::waitForPmuPowerRemoval);
  assert(shutdownFallbackAction(true, true, true) ==
         ShutdownFallbackAction::deepSleepWithBootWake);
  assert(shutdownFallbackAction(false, true, true) ==
         ShutdownFallbackAction::deepSleepWithBootWake);
  assert(shutdownFallbackAction(false, true, false) ==
         ShutdownFallbackAction::deepSleepWithoutWake);

  // DTR owns a host session while physical USB remains a separate fact.
  UsbCdcSessionState cdc;
  cdc.lineState(true);
  assert(cdc.active());
  cdc.lineState(false);
  assert(!cdc.active());
  assert(cdc.takeClosed());
  assert(usbPhysicalConnected(true, true, true));
  assert(!usbPhysicalConnected(true, true, false));
  return 0;
}
