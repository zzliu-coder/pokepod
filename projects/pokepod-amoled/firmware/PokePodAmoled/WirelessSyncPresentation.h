#pragma once

#include <stdint.h>

namespace pokepod {

enum class WirelessSyncPresentationPhase : uint8_t {
  unpaired,
  idle,
  opening,
  waiting,
  authenticating,
  syncing,
  completed,
  failed,
};

struct WirelessSyncPresentationInput {
  bool secureReady = false;
  bool paired = false;
  bool windowOpen = false;
  bool networkConnected = false;
  bool listenerActive = false;
  bool bonjourActive = false;
  bool clientConnected = false;
  bool authenticated = false;
  bool linkBusy = false;
  bool completed = false;
  bool hasError = false;
  uint32_t remainingSeconds = 0;
};

inline WirelessSyncPresentationPhase wirelessSyncPresentationPhase(
    const WirelessSyncPresentationInput &input) {
  if (!input.secureReady || !input.paired) {
    return WirelessSyncPresentationPhase::unpaired;
  }
  if (!input.windowOpen) return WirelessSyncPresentationPhase::idle;
  if (input.hasError) return WirelessSyncPresentationPhase::failed;
  if (!input.networkConnected || !input.listenerActive ||
      !input.bonjourActive) {
    return WirelessSyncPresentationPhase::opening;
  }
  if (input.clientConnected && !input.authenticated) {
    return WirelessSyncPresentationPhase::authenticating;
  }
  if (input.authenticated && input.linkBusy) {
    return WirelessSyncPresentationPhase::syncing;
  }
  if (input.completed) return WirelessSyncPresentationPhase::completed;
  return WirelessSyncPresentationPhase::waiting;
}

enum class ComputerSyncEntryDecision : uint8_t {
  openAndNavigate,
  navigateOnly,
};

inline ComputerSyncEntryDecision computerSyncEntryDecision(bool windowOpen) {
  return windowOpen ? ComputerSyncEntryDecision::navigateOnly
                    : ComputerSyncEntryDecision::openAndNavigate;
}

}  // namespace pokepod
