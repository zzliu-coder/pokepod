#include <assert.h>

#include "../PokePodAmoled/RecorderStartState.h"

using namespace pokepod;

int main() {
  RecorderStartState start;
  assert(start.begin(299000));
  assert(start.poll(299999, true, false, false) ==
         RecorderStartPollResult::pending);
  // A storage ACK arriving at the exact five-minute transfer deadline cannot
  // publish a successful Link recording start.
  assert(start.poll(300000, false, true, true) ==
         RecorderStartPollResult::cancelled);
  assert(!start.active());

  assert(start.begin(10));
  assert(start.poll(11, true, true, true) ==
         RecorderStartPollResult::started);
  assert(start.begin(20));
  assert(start.poll(21, true, true, false) ==
         RecorderStartPollResult::failed);

  // USB has no absolute time-window gate while connected. DTR close is
  // represented by the Link-owned cancellation gate becoming false.
  assert(start.begin(30));
  assert(start.poll(31, false, false, false) ==
         RecorderStartPollResult::cancelled);
  assert(start.begin(40));
  assert(start.poll(5040, true, false, false) ==
         RecorderStartPollResult::cancelled);
  return 0;
}
