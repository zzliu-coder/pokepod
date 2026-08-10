#include <cassert>

#include "../PokePodAmoled/WirelessSyncWindow.h"

int main() {
  using namespace pokepod;
  WirelessSyncWindow window;
  WirelessSyncWindowInputs input;
  assert(window.update(1, input).phase == WirelessSyncWindowPhase::closed);

  window.open(100);
  assert(window.remainingMs(100) == kWirelessSyncWindowMs);
  auto result = window.update(100, input);
  assert(result.wifiDemand && !result.listener && !result.bonjour);
  assert(result.phase == WirelessSyncWindowPhase::waitingForNetwork);

  input.networkConnected = true;
  input.secureServerReady = true;
  result = window.update(101, input);
  assert(result.listener && result.bonjour);
  assert(result.phase == WirelessSyncWindowPhase::discoverable);

  input.clientConnected = true;
  result = window.update(102, input);
  assert(result.phase == WirelessSyncWindowPhase::connected);

  const uint32_t fixedDeadline = window.deadlineMs();
  result = window.update(100 + kWirelessSyncWindowMs - 1, input);
  assert(result.phase == WirelessSyncWindowPhase::connected);
  assert(result.remainingMs == 1);
  assert(window.remainingMs(100 + kWirelessSyncWindowMs - 1) == 1);
  assert(window.deadlineMs() == fixedDeadline);

  result = window.update(100 + kWirelessSyncWindowMs, input);
  assert(result.phase == WirelessSyncWindowPhase::closed);
  assert(!result.wifiDemand && !result.listener && !result.bonjour);
  assert(window.remainingMs(100 + kWirelessSyncWindowMs) == 0);

  window.open(0);
  assert(!window.deadlineReached(kWirelessSyncWindowMs - 1));
  assert(window.deadlineReached(kWirelessSyncWindowMs));
  result = window.update(kWirelessSyncWindowMs, input);
  assert(result.phase == WirelessSyncWindowPhase::closed);

  window.open(2000);
  window.close();
  assert(window.update(2001, input).phase == WirelessSyncWindowPhase::closed);
  return 0;
}
