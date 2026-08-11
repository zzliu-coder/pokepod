#include <assert.h>

#include "../PokePodAmoled/LinkRecordingStop.h"

using namespace pokepod;

namespace {

struct SessionHarness {
  LinkRecordingStop stop;
  bool routerOwned = true;
  bool recorderActive = true;
  bool terminalAvailable = false;
  bool terminalSuccess = false;
  bool responded = false;
  bool queued = false;
  bool gatePermitted = true;

  void poll() {
    if (!stop.awaitsRecorder() || recorderActive || !terminalAvailable) return;
    queued = terminalSuccess;
    responded = stop.shouldRespond() && gatePermitted;
    routerOwned = false;
    stop.finish();
  }
};

}  // namespace

int main() {
  LinkRecordingStop stop;
  assert(stop.begin(41, true, true));
  assert(stop.active());
  assert(stop.awaitsCapture());
  assert(stop.ownsRequest(41));
  assert(!stop.begin(42, true, true));

  // A global capture poll may consume the late task acknowledgement.  The
  // Link layer observes the resulting stopped fact and advances its owner.
  stop.captureFinalized();
  assert(stop.awaitsRecorder());

  // Disconnect suppresses a response and forces abort semantics while the
  // recorder cleanup still owns the session.
  stop.suppressResponseAndAbort();
  assert(!stop.shouldRespond());
  assert(!stop.commitRequested());
  assert(stop.active());

  stop.finish();
  assert(!stop.active());
  assert(stop.begin(43, true, true));
  stop.captureFinalized();
  assert(stop.awaitsRecorder());
  stop.finish();

  // A stop request retains router ownership until the recorder's durable
  // terminal result is available; a queued stop is not a commit fact.
  SessionHarness success;
  assert(success.stop.begin(50, true, true));
  success.stop.captureFinalized();
  success.poll();
  assert(success.routerOwned);
  assert(success.stop.active());
  success.recorderActive = false;
  success.terminalAvailable = true;
  success.terminalSuccess = true;
  success.poll();
  assert(!success.routerOwned);
  assert(success.queued);
  assert(success.responded);

  // A CDC close, quiesce, queue overflow, or wireless 299->300 second
  // deadline all use the same abort/suppress path. Cleanup continues, the
  // terminal failure is consumed, and a second session is admitted only then.
  SessionHarness cancelled;
  assert(cancelled.stop.begin(60, true, true));
  cancelled.stop.suppressResponseAndAbort();
  cancelled.stop.captureFinalized();
  cancelled.gatePermitted = false;
  cancelled.poll();
  assert(cancelled.routerOwned);
  assert(!cancelled.stop.begin(61, true, true));
  cancelled.recorderActive = false;
  cancelled.terminalAvailable = true;
  cancelled.terminalSuccess = false;
  cancelled.poll();
  assert(!cancelled.routerOwned);
  assert(!cancelled.queued);
  assert(!cancelled.responded);
  assert(cancelled.stop.begin(61, true, true));
  cancelled.stop.finish();
  return 0;
}
