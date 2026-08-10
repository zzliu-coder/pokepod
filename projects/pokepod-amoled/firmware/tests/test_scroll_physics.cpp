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

  // A slow UI frame must advance the same physical clock instead of silently
  // discarding everything after the former 50 ms clamp.
  ScrollPhysics stepped;
  ScrollPhysics delayed;
  stepped.setMaximum(1000);
  delayed.setMaximum(1000);
  assert(stepped.beginDrag(200, 3000));
  assert(delayed.beginDrag(200, 3000));
  assert(stepped.dragTo(170, 3020));
  assert(delayed.dragTo(170, 3020));
  stepped.endDrag(3020);
  delayed.endDrag(3020);
  for (uint32_t now = 3040; now <= 3180; now += 20) stepped.tick(now);
  assert(delayed.tick(3180));
  const int32_t positionDifference = stepped.positionPx() - delayed.positionPx();
  assert(positionDifference >= -3 && positionDifference <= 3);
  const int32_t velocityDifference =
      stepped.velocityPxPerSecond() - delayed.velocityPxPerSecond();
  assert(velocityDifference >= -3 && velocityDifference <= 3);
  return 0;
}
