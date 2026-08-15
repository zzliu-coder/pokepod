#include <assert.h>
#include <stdint.h>

#include "../PokePodAmoled/LinkPollBudget.h"

using namespace pokepod;

namespace {
uint64_t fakeClock(void *context) {
  return *static_cast<uint64_t *>(context);
}
}  // namespace

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

  // One slow phase exhausts the shared poll gate. Later cleanup/dispatch
  // primitives cannot run in that same poll.
  uint64_t now = 1000;
  LinkPollBudget phaseBudget(100, 20, now);
  LinkPollPhaseGate gate(phaseBudget, fakeClock, &now);
  unsigned first = 0;
  unsigned later = 0;
  assert(!gate.run([&]() {
    ++first;
    now += 20;
  }));
  assert(first == 1);
  assert(!gate.run([&]() { ++later; }));
  assert(later == 0);

  // processFrame uses the same before-dispatch checkpoint: a parser phase
  // that consumes the remaining time leaves dispatch for the next poll.
  now = 2000;
  LinkPollBudget dispatchBudget(100, 10, now);
  LinkPollPhaseGate dispatch(dispatchBudget, fakeClock, &now);
  unsigned parsed = 0;
  unsigned dispatched = 0;
  assert(!dispatch.run([&]() {
    ++parsed;
    now += 10;
  }));
  assert(parsed == 1);
  assert(!dispatch.run([&]() { ++dispatched; }));
  assert(dispatched == 0);
  return 0;
}
