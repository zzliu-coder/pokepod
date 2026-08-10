#include <assert.h>

#include "PageTransition.h"

using namespace pokepod;

int main() {
  PageTransition transition;
  transition.prepare(PageTransitionDirection::fromRight);
  PageTransitionRegion region = transition.begin(1000, 368);
  assert(region.x == 336 && region.width == 32);
  region = transition.advance(1070);
  assert(region.valid() && region.x + region.width == 336);
  region = transition.advance(1140);
  assert(region.x == 0 && !transition.active());

  transition.prepare(PageTransitionDirection::fromLeft);
  region = transition.begin(0xfffffff0U, 368);
  assert(region.x == 0 && region.width == 32);
  region = transition.advance(54);
  assert(region.x == 32 && region.width > 0);
  region = transition.advance(140);
  assert(!transition.active());

  transition.prepare(PageTransitionDirection::fromRight);
  transition.cancel();
  assert(!transition.running());
  return 0;
}
