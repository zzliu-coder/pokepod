#include <cassert>
#include <cstdint>

#include "PowerPolicy.h"

using namespace pokepod;

int main() {
  PowerInputs input;
  PowerDecision decision = decidePower(input);
  assert(decision.mode == PowerMode::balanced);
  assert(decision.cpuMhz == 80);
  assert(!decision.allowLightSleep);

  input.audioActive = true;
  decision = decidePower(input);
  assert(decision.mode == PowerMode::performance);
  assert(decision.cpuMhz == 240);

  input.audioActive = false;
  input.screenOn = false;
  decision = decidePower(input);
  assert(decision.mode == PowerMode::screenOffIdle);
  assert(!decision.allowLightSleep);

  input.idleMs = kLightSleepTimeoutMs;
  input.bleConnected = true;
  decision = decidePower(input);
  assert(decision.mode == PowerMode::screenOffIdle);
  assert(!decision.allowLightSleep);
  assert(decision.requestIdleRadioPause);

  input.bleConnected = false;
  assert(!decidePower(input).allowLightSleep);
  input.automaticWakeEnabled = false;
  decision = decidePower(input);
  assert(decision.mode == PowerMode::lightSleep);
  assert(decision.allowLightSleep);
  assert(decision.lightSleepTimerUs == 120000000ULL);
  input.usbHostConnected = true;
  assert(!decidePower(input).allowLightSleep);
  input.usbHostConnected = false;
  input.vbusPresent = true;
  decision = decidePower(input);
  assert(decision.allowLightSleep);
  assert(decision.lightSleepTimerUs == 30000000ULL);
  input.vbusPresent = false;
  input.automaticWakeEnabled = true;
  input.wifiRadioOn = true;
  decision = decidePower(input);
  assert(decision.mode == PowerMode::screenOffIdle);
  assert(decision.cpuMhz == 80);
  assert(!decision.allowLightSleep);
  input.wifiRadioOn = false;
  input.networkBusy = true;
  assert(decidePower(input).mode == PowerMode::performance);
  input.networkBusy = false;
  input.provisioning = true;
  assert(decidePower(input).mode == PowerMode::performance);
  input.provisioning = false;
  input.uiAnimating = true;
  assert(decidePower(input).mode == PowerMode::performance);
  assert(decidePower(input).cpuMhz == 240);
  input.uiAnimating = false;

  input.idleMs = kDeepSleepTimeoutMs;
  input.wifiRadioOn = true;
  decision = decidePower(input);
  assert(decision.mode == PowerMode::deepSleepPending);
  assert(decision.requestDeepSleep);
  assert(decision.requestIdleRadioPause);
  input.vbusPresent = true;
  assert(!decidePower(input).requestDeepSleep);
  input.vbusPresent = false;
  input.criticalBattery = true;
  input.idleMs = 1;
  decision = decidePower(input);
  assert(decision.mode == PowerMode::safeShutdownPending);
  assert(decision.requestSafeShutdown);
  input.audioActive = true;
  assert(decidePower(input).mode == PowerMode::performance);

  LowBatteryShutdownPolicy battery;
  assert(!battery.update(5, false));
  assert(!battery.update(5, false));
  assert(battery.update(5, false));
  assert(battery.critical());
  assert(!battery.update(5, true));
  assert(!battery.critical());
  assert(!battery.update(6, false));
  assert(!battery.update(7, false));

  AutoScreenOffPolicy autoOff;
  autoOff.begin(0xfffffff0U);
  assert(!autoOff.shouldTurnOff(0x00000005U, true, false, 32));
  assert(autoOff.shouldTurnOff(0x00000020U, true, false, 32));
  assert(!autoOff.shouldTurnOff(0x00000020U, true, true, 32));
  autoOff.noteActivity(100);
  assert(!autoOff.shouldTurnOff(129, true, false, 30));
  assert(autoOff.shouldTurnOff(130, true, false, 30));
  autoOff.noteActivity(1001);
  assert(autoOff.idleMs(1000) == 0);
  assert(!autoOff.shouldTurnOff(1000, true, false, 30));
  assert(!autoOff.shouldTurnOff(1001, true, false, 30));
  assert(autoOff.shouldTurnOff(1031, true, false, 30));
  autoOff.noteActivity(2000);
  assert(!autoOff.shouldDim(2011, true, false, 12));
  assert(autoOff.shouldDim(2012, true, false, 12));
  assert(!autoOff.shouldDim(2012, true, true, 12));
  return 0;
}
