#include <assert.h>

#include "../PokePodAmoled/LocalRecordingStart.h"

using namespace pokepod;

int main() {
  LocalRecordingStartState state;
  uint32_t sessionId = 99;

  assert(!state.begin(0));
  assert(state.observe(RecorderStartPollResult::pending, sessionId) ==
         LocalRecordingStartAction::idle);
  assert(sessionId == 0);

  assert(state.begin(42));
  assert(!state.begin(43));
  assert(state.gatePermitted());
  assert(state.captureSessionId() == 42);
  assert(state.observe(RecorderStartPollResult::pending, sessionId) ==
         LocalRecordingStartAction::waiting);
  assert(state.observe(RecorderStartPollResult::started, sessionId) ==
         LocalRecordingStartAction::startCapture);
  assert(sessionId == 42);
  assert(!state.active());

  assert(state.begin(77));
  state.requestCancel();
  assert(!state.gatePermitted());
  assert(state.observe(RecorderStartPollResult::cancelled, sessionId) ==
         LocalRecordingStartAction::cleanup);
  assert(sessionId == 77);
  assert(!state.active());

  assert(state.begin(88));
  assert(state.observe(RecorderStartPollResult::failed, sessionId) ==
         LocalRecordingStartAction::cleanup);
  assert(sessionId == 88);
  return 0;
}
