#include <cassert>

#include "ProvisioningRadioPolicy.h"

using namespace pokepod;

int main() {
  assert(provisioningRadioReady(true, true, false));
  assert(!provisioningRadioReady(false, true, false));
  assert(!provisioningRadioReady(true, false, false));
  assert(!provisioningRadioReady(true, true, true));
  return 0;
}
