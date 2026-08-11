#include <assert.h>

#include "AudioCaptureSessionState.h"

using namespace pokepod;

int main() {
  AudioCaptureSessionState state;
  assert(!state.busy());
  assert(state.begin());
  assert(state.captureActive());
  assert(!state.begin());

  // A main-loop wait may time out here. Ownership remains busy and a second
  // recording cannot start while the realtime task is finishing late.
  assert(state.requestStop());
  assert(state.stopRequested());
  assert(state.busy());
  assert(!state.begin());
  assert(state.requestStop());

  // The realtime task reports its late stop; only main-loop finalization
  // returns the service to idle and permits the next session.
  assert(state.taskStopped());
  assert(state.finalizePending());
  assert(!state.begin());
  // The task changes phase before it signals the main-loop semaphore. An
  // early poll cannot finalize and leave a stale stop token for session two.
  assert(!state.finishFinalize());
  assert(state.acknowledgeTaskStopped());
  assert(state.finishFinalize());
  assert(!state.busy());
  assert(state.begin());
  assert(state.requestStop());
  assert(state.taskStopped());
  assert(!state.finishFinalize());
  assert(state.acknowledgeTaskStopped());
  assert(state.finishFinalize());
  assert(!state.busy());

  assert(!state.taskStopped());
  assert(!state.finishFinalize());
  return 0;
}
