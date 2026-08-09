#include <assert.h>
#include <stdint.h>

#include "ScrollPhysics.h"

int main() {
  using pokepod::ScrollPhysics;

  ScrollPhysics scroll;
  assert(!scroll.beginDrag(200, 0));
  assert(scroll.setMaximum(500) == false);
  assert(scroll.beginDrag(200, 100));
  assert(scroll.dragTo(188, 110));
  assert(scroll.positionPx() == 12);
  assert(scroll.dragTo(150, 130));
  assert(scroll.positionPx() == 50);
  assert(scroll.velocityPxPerSecond() > 0);
  scroll.endDrag(130);
  assert(scroll.coasting());
  const int32_t beforeCoast = scroll.positionPx();
  assert(scroll.tick(146));
  assert(scroll.positionPx() > beforeCoast);
  for (uint32_t now = 162; now < 3000 && scroll.coasting(); now += 16) {
    scroll.tick(now);
  }
  assert(!scroll.coasting());
  assert(scroll.positionPx() <= scroll.maximumPx());

  scroll.reset();
  scroll.setMaximum(80);
  assert(scroll.beginDrag(100, 0xfffffff0U));
  assert(scroll.dragTo(0, 4));
  assert(scroll.positionPx() == 80);
  scroll.endDrag(4);
  assert(!scroll.coasting());
  assert(scroll.setMaximum(30));
  assert(scroll.positionPx() == 30);

  scroll.reset();
  scroll.setMaximum(300);
  assert(scroll.beginDrag(200, 1000));
  assert(scroll.dragTo(160, 1020));
  scroll.endDrag(1120);
  assert(!scroll.coasting());
  assert(scroll.positionPx() == 40);

  scroll.reset();
  scroll.setMaximum(300);
  assert(scroll.beginDrag(100, 2000));
  assert(!scroll.dragTo(140, 2020));
  assert(scroll.positionPx() == 0);
  scroll.endDrag(2020);
  assert(!scroll.coasting());
  return 0;
}
