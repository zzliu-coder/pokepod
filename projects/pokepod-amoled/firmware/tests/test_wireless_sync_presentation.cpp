#include <cassert>

#include "../PokePodAmoled/WirelessSyncPresentation.h"

int main() {
  using namespace pokepod;
  WirelessSyncPresentationInput input;
  assert(wirelessSyncPresentationPhase(input) ==
         WirelessSyncPresentationPhase::unpaired);

  input.secureReady = true;
  input.paired = true;
  assert(wirelessSyncPresentationPhase(input) ==
         WirelessSyncPresentationPhase::idle);

  input.windowOpen = true;
  input.remainingSeconds = 300;
  assert(wirelessSyncPresentationPhase(input) ==
         WirelessSyncPresentationPhase::opening);

  input.networkConnected = true;
  input.listenerActive = true;
  input.bonjourActive = true;
  assert(wirelessSyncPresentationPhase(input) ==
         WirelessSyncPresentationPhase::waiting);

  input.clientConnected = true;
  assert(wirelessSyncPresentationPhase(input) ==
         WirelessSyncPresentationPhase::authenticating);

  input.authenticated = true;
  input.linkBusy = true;
  assert(wirelessSyncPresentationPhase(input) ==
         WirelessSyncPresentationPhase::syncing);

  input.linkBusy = false;
  input.completed = true;
  assert(wirelessSyncPresentationPhase(input) ==
         WirelessSyncPresentationPhase::completed);

  input.completed = false;
  input.hasError = true;
  assert(wirelessSyncPresentationPhase(input) ==
         WirelessSyncPresentationPhase::failed);

  input.hasError = false;
  input.completed = true;
  input.linkBusy = true;
  assert(wirelessSyncPresentationPhase(input) ==
         WirelessSyncPresentationPhase::syncing);

  input.completed = false;
  input.linkBusy = false;
  input.clientConnected = false;
  input.authenticated = false;
  input.bonjourActive = false;
  assert(wirelessSyncPresentationPhase(input) ==
         WirelessSyncPresentationPhase::opening);

  assert(computerSyncEntryDecision(false) ==
         ComputerSyncEntryDecision::openAndNavigate);
  assert(computerSyncEntryDecision(true) ==
         ComputerSyncEntryDecision::navigateOnly);
  return 0;
}
