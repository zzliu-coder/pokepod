#include <cassert>

#include "../PokePodAmoled/UsbLinkSessionReconcile.h"

using namespace pokepod;

int main() {
  assert(!usbLinkMagicRequiresEpochReset(false, 0, 0, 8));
  assert(!usbLinkMagicRequiresEpochReset(true, 3, 8, 8));
  assert(usbLinkMagicRequiresEpochReset(true, 3, 7, 8));
  assert(usbLinkMagicRequiresEpochReset(false, 3, 0, 8));
  assert(!usbLinkMagicRequiresEpochReset(true, 3, 7, 0));

  assert(usbLinkSessionAction(true, false, false, true, 8, 8, 7) ==
         UsbLinkSessionAction::none);
  assert(usbLinkSessionAction(true, false, false, true, 8, 7, 7) ==
         UsbLinkSessionAction::disconnectRetainingNewBytes);
  assert(usbLinkSessionAction(true, false, false, true, 8, 0, 7) ==
         UsbLinkSessionAction::disconnectRetainingNewBytes);
  assert(usbLinkSessionAction(true, false, true, false, 8, 8, 8) ==
         UsbLinkSessionAction::disconnectAndDiscard);
  assert(usbLinkSessionAction(true, true, false, false, 8, 8, 8) ==
         UsbLinkSessionAction::disconnectAndDiscard);
  assert(usbLinkSessionAction(true, false, false, true, 1, 0, 0) ==
         UsbLinkSessionAction::none);
  return 0;
}
