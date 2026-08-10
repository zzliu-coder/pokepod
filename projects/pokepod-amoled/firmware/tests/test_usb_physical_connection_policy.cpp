#include <cassert>

#include "UsbPhysicalConnectionPolicy.h"

using namespace pokepod;

int main() {
  assert(usbPhysicalConnected(true, true, true));
  assert(!usbPhysicalConnected(true, true, false));
  assert(!usbPhysicalConnected(false, true, true));
  assert(usbPhysicalConnected(true, false, false));
  return 0;
}
