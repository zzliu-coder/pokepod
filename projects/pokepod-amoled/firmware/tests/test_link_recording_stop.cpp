#include <assert.h>

#include "../PokePodAmoled/LinkRecordingStop.h"

using namespace pokepod;

int main() {
  LinkRecordingStop stop;
  assert(stop.begin(41, true, true));
  assert(stop.active());
  assert(stop.awaitsCapture());
  assert(stop.ownsRequest(41));
  assert(!stop.begin(42, true, true));

  // A global capture poll may consume the late task acknowledgement.  The
  // Link layer observes the resulting stopped fact and advances its owner.
  stop.captureFinalized(false);
  assert(stop.awaitsRecorder());
  assert(!stop.recorderSucceeded());

  // Disconnect suppresses a response and forces abort semantics while the
  // recorder cleanup still owns the session.
  stop.suppressResponseAndAbort();
  assert(!stop.shouldRespond());
  assert(!stop.commitRequested());
  assert(stop.active());

  stop.finish();
  assert(!stop.active());
  assert(stop.begin(43, true, true));
  stop.captureFinalized(true);
  assert(stop.recorderSucceeded());
  stop.finish();
  return 0;
}
