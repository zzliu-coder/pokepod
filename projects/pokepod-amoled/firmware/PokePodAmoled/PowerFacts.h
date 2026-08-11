#pragma once

namespace pokepod {

// Raw hardware/service facts.  No field in this snapshot implies a power
// policy by itself: TinyUSB mount, CDC ownership, VBUS and charging are four
// different observations and must remain distinguishable.
struct PowerFacts {
  bool usbMounted = false;
  bool cdcSessionActive = false;
  bool vbusPresent = false;
  bool charging = false;
  bool linkLeaseActive = false;
  bool bleRadioActive = false;
  bool bleStreaming = false;
  bool wifiRadioActive = false;
  bool wakeSourcesReady = false;
  bool storageMutationActive = false;
};

inline bool powerHostSessionActive(const PowerFacts &facts) {
  return facts.cdcSessionActive || facts.linkLeaseActive;
}

inline bool shouldArmAutomaticWake(bool automaticWakeEnabled,
                                   const PowerFacts &facts) {
  return automaticWakeEnabled && facts.wakeSourcesReady;
}

}  // namespace pokepod
