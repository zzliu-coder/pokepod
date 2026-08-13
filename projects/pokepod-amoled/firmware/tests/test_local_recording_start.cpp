#include <assert.h>

#include "../PokePodAmoled/LocalRecordingStart.h"

using namespace pokepod;

int main() {
  // A Link recording uses the same router enum but owns a distinct recorder
  // operation. The local App must reject admission and must never consume or
  // release Link's terminal resources.
  assert(!LocalRecordingOwnershipPolicy::mayAdmit(false, true, true));
  assert(!LocalRecordingOwnershipPolicy::mayManageFailure(false));
  assert(!LocalRecordingOwnershipPolicy::mayReleaseRouter(false));
  assert(LocalRecordingOwnershipPolicy::mayAdmit(true, false, false));
  assert(LocalRecordingOwnershipPolicy::mayManageFailure(true));
  assert(LocalRecordingOwnershipPolicy::mayReleaseRouter(true));

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
