#include <cassert>
#include <cstdint>

#include "../PokePodAmoled/WifiPolicy.h"

int main() {
  using namespace pokepod;
  WifiDecision state;
  WifiInputs input;
  state = nextWifiDecision(state, input, 0);
  assert(state.phase == WifiPhase::disabled && !state.radioOn);

  input.configured = true;
  state = nextWifiDecision(state, input, 100);
  assert(state.phase == WifiPhase::off && !state.radioOn);
  input.recording = true;
  state = nextWifiDecision(state, input, 200);
  assert(state.phase == WifiPhase::connecting && state.radioOn);
  input.connected = true;
  input.pendingWork = true;
  state = nextWifiDecision(state, input, 300);
  assert(state.phase == WifiPhase::online && state.processQueue);

  input.recording = false;
  input.pendingWork = false;
  state = nextWifiDecision(state, input, 1000);
  assert(state.phase == WifiPhase::grace && state.idleSinceMs == 1000);
  state = nextWifiDecision(state, input, 1000 + kWifiGraceMs - 1);
  assert(state.radioOn);
  state = nextWifiDecision(state, input, 1000 + kWifiGraceMs);
  assert(state.phase == WifiPhase::off && !state.radioOn);

  input.connected = false;
  input.manualWake = true;
  state = nextWifiDecision(state, input, 390000);
  assert(state.phase == WifiPhase::connecting && state.radioOn);
  input.manualWake = false;

  input.wirelessSync = true;
  state = nextWifiDecision(state, input, 400000);
  assert(state.phase == WifiPhase::connecting && state.radioOn);
  input.connected = true;
  state = nextWifiDecision(state, input, 400001);
  assert(state.phase == WifiPhase::online && state.radioOn);
  assert(!state.processQueue);
  input.wirelessSync = false;
  input.connected = false;

  input.charging = true;
  state = nextWifiDecision(state, input, 500000);
  assert(state.radioOn);
  input.manuallyDisabled = true;
  state = nextWifiDecision(state, input, 500001);
  assert(state.phase == WifiPhase::off && !state.radioOn);
  input.wirelessSync = true;
  state = nextWifiDecision(state, input, 500002);
  assert(state.phase == WifiPhase::connecting && state.radioOn);
  input.wirelessSync = false;
  input.provisioning = true;
  state = nextWifiDecision(state, input, 500003);
  assert(state.phase == WifiPhase::provisioning && state.radioOn);

  assert(wifiRetryDelayMs(0) == 10000);
  assert(wifiRetryDelayMs(1) == 30000);
  assert(wifiRetryDelayMs(2) == 120000);
  assert(wifiRetryDelayMs(3) == 0);
  return 0;
}
