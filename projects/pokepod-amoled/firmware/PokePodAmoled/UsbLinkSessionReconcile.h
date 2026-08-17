#pragma once

#include <stdint.h>

namespace pokepod {

enum class UsbLinkSessionAction : uint8_t {
  none = 0,
  disconnectAndDiscard,
  disconnectRetainingNewBytes,
};

// The DTR epoch can advance after App sampled it but before Link consumes the
// next PPV2 magic.  In that window the parser itself is the first component
// that can prove a new physical host session.  It must retire the old logical
// owner before admitting bytes from the new epoch.
inline bool usbLinkMagicRequiresEpochReset(
    bool sessionActive, uint32_t linkGeneration,
    uint32_t boundUsbGeneration, uint32_t observedUsbGeneration) {
  if (observedUsbGeneration == 0) return false;
  if (!sessionActive && linkGeneration == 0 && boundUsbGeneration == 0) {
    return false;
  }
  return boundUsbGeneration != observedUsbGeneration;
}

// A new PPV2 request may arrive after App sampled DTR but before Link::poll().
// If Link already bound that request to the new DTR epoch, the next loop must
// not tear it down merely because App is observing the generation one turn
// late.
inline UsbLinkSessionAction usbLinkSessionAction(
    bool linkStarted, bool physicallyDisconnected,
    bool currentSessionClosed, bool sessionAdvanced,
    uint32_t currentUsbGeneration, uint32_t linkBoundUsbGeneration,
    uint32_t previousUsbGeneration) {
  if (!linkStarted) return UsbLinkSessionAction::none;
  if (physicallyDisconnected || currentSessionClosed) {
    return UsbLinkSessionAction::disconnectAndDiscard;
  }
  if (!sessionAdvanced || previousUsbGeneration == 0) {
    return UsbLinkSessionAction::none;
  }
  if (currentUsbGeneration != 0 &&
      linkBoundUsbGeneration == currentUsbGeneration) {
    return UsbLinkSessionAction::none;
  }
  return UsbLinkSessionAction::disconnectRetainingNewBytes;
}

}  // namespace pokepod
