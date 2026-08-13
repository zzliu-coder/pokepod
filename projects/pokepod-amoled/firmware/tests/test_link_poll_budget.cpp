#include <assert.h>
#include <stdint.h>

#include "../PokePodAmoled/LinkPollBudget.h"

using namespace pokepod;

int main() {
  LinkPollBudget bytesFirst(4, 100, 1000);
  assert(bytesFirst.permits(1000));
  bytesFirst.consume(3);
  assert(bytesFirst.bytesUsed() == 3);
  assert(bytesFirst.bytesRemaining() == 1);
  assert(bytesFirst.permits(1099));
  bytesFirst.consume();
  assert(!bytesFirst.permits(1099));

  LinkPollBudget timeFirst(100, 25, 500);
  timeFirst.consume(2);
  assert(timeFirst.permits(524));
  assert(!timeFirst.permits(525));
  assert(timeFirst.bytesRemaining() == 98);

  LinkPollBudget exactByteBoundary(8, 10, 0);
  exactByteBoundary.consume(8);
  assert(!exactByteBoundary.permits(9));

  // esp_timer_get_time is monotonic. A backwards fake-clock jump fails closed
  // and can never grant a second full slice.
  LinkPollBudget backwards(8, 8, 100);
  assert(!backwards.permits(99));

  LinkPollBudget largeClock(8, 8, UINT64_MAX - 9);
  assert(largeClock.permits(UINT64_MAX - 2));
  assert(!largeClock.permits(UINT64_MAX - 1));

  LinkPollBudget saturated(3, 10, 0);
  saturated.consume(100);
  assert(saturated.bytesUsed() == 3);
  assert(saturated.bytesRemaining() == 0);
  return 0;
}
