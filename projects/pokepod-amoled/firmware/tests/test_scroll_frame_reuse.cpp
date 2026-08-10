#include <assert.h>

#include "../PokePodAmoled/ScrollFrameReuse.h"

using pokepod::scrollFrameReusePlan;

int main() {
  auto down = scrollFrameReusePlan(10, 30, 100);
  assert(down.valid && down.sourceRow == 20 && down.destinationRow == 0);
  assert(down.moveRows == 80 && down.exposedRow == 80 &&
         down.exposedRows == 20);

  auto up = scrollFrameReusePlan(30, 10, 100);
  assert(up.valid && up.sourceRow == 0 && up.destinationRow == 20);
  assert(up.moveRows == 80 && up.exposedRow == 0 && up.exposedRows == 20);

  auto same = scrollFrameReusePlan(10, 10, 100);
  assert(same.valid && same.moveRows == 100 && same.exposedRows == 0);
  assert(!scrollFrameReusePlan(0, 100, 100).valid);
  assert(!scrollFrameReusePlan(100, 0, 100).valid);
  return 0;
}
