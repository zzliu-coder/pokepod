#include <cassert>
#include <cmath>

#include "../PokePodAmoled/RaiseToWakePolicy.h"

int main() {
  using namespace pokepod;
  RaiseToWakePolicy policy;
  assert(!policy.update(0, true, false, 0.0f, 0.0f, 1.0f));
  assert(!policy.update(100, true, false, 0.05f, 0.0f, 0.99f));
  assert(policy.update(200, true, false, 0.45f, 0.0f, 0.89f));
  assert(!policy.update(300, true, false, 0.0f, 0.0f, 1.0f));

  RaiseToWakePolicy disabled;
  disabled.update(0, false, false, 0.0f, 0.0f, 1.0f);
  assert(!disabled.update(100, false, false, 0.5f, 0.0f, 0.86f));

  RaiseToWakePolicy screenOn;
  screenOn.update(0, true, true, 0.0f, 0.0f, 1.0f);
  assert(!screenOn.update(100, true, true, 0.5f, 0.0f, 0.86f));
  assert(!screenOn.update(200, true, false, NAN, 0.0f, 1.0f));
  return 0;
}
