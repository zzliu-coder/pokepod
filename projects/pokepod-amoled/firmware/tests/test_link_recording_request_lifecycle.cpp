#include <assert.h>

#include "../PokePodAmoled/LinkRecordingStart.h"
#include "../PokePodAmoled/LinkRecordingStop.h"
#include "../PokePodAmoled/RecorderStartState.h"

using namespace pokepod;

namespace {

struct LifecycleHarness {
  LinkRecordingStart request;
  LinkRecordingStop cleanup;
  RecorderStartState storage;
  bool leaseHeld = false;
  bool completed = false;
  bool responded = false;
  unsigned starts = 0;

  bool accept(uint32_t requestId, uint32_t sessionId, uint32_t nowMs) {
    if (request.ownsRequest(requestId)) return true;
    if (request.active() || !request.begin(requestId, sessionId) ||
        !storage.begin(nowMs)) return false;
    leaseHeld = true;
    ++starts;
    return true;
  }

  bool blocksOtherRequest(uint32_t requestId) const {
    if (!request.active() && !cleanup.active()) return false;
    return !request.ownsRequest(requestId) &&
        !cleanup.ownsRequest(requestId);
  }

  void pollStart(uint32_t nowMs, bool gate, bool ack, bool success,
                 bool transportAlive = true) {
    const RecorderStartPollResult result =
        storage.poll(nowMs, gate, ack, success);
    if (result == RecorderStartPollResult::pending) return;
    const uint32_t requestId = request.requestId();
    request.finish();
    if (result == RecorderStartPollResult::started && transportAlive) {
      responded = true;
      completed = true;
      leaseHeld = false;
      return;
    }
    assert(cleanup.begin(requestId, false, transportAlive));
    cleanup.captureFinalized();
  }

  void finishCleanup(bool transportAlive) {
    assert(cleanup.awaitsRecorder());
    const uint32_t requestId = cleanup.requestId();
    const bool respond = cleanup.shouldRespond() && transportAlive;
    cleanup.finish();
    responded = respond;
    completed = respond && requestId != 0;
    leaseHeld = false;
  }
};

}  // namespace

int main() {
  LinkRecordingStart staged;
  assert(staged.begin(69, 690));
  assert(!staged.recorderRequested());
  assert(staged.markRecorderRequested());
  assert(staged.recorderRequested());
  assert(!staged.markRecorderRequested());
  staged.finish();
  assert(!staged.recorderRequested());

  // Dispatch acceptance is not completion. A duplicate frame is coalesced
  // into the same in-flight request and cannot start a second recorder.
  LifecycleHarness success;
  assert(success.accept(70, 700, 100));
  assert(success.accept(70, 701, 101));
  assert(success.starts == 1);
  assert(success.blocksOtherRequest(90));
  assert(success.leaseHeld);
  success.pollStart(102, true, false, false);
  assert(success.leaseHeld);
  assert(!success.completed);
  assert(!success.responded);
  success.pollStart(103, true, true, true);
  assert(success.completed);
  assert(success.responded);
  assert(!success.leaseHeld);
  assert(!success.blocksOtherRequest(90));

  // A negative storage ACK remains owned until recorder cleanup reaches its
  // terminal fact; only then is the failure response/completion published.
  LifecycleHarness failed;
  assert(failed.accept(71, 710, 200));
  failed.pollStart(201, true, true, false);
  assert(failed.leaseHeld);
  assert(!failed.completed);
  assert(failed.cleanup.ownsRequest(71));
  assert(failed.blocksOtherRequest(91));
  assert(failed.leaseHeld);
  failed.finishCleanup(true);
  assert(failed.completed);
  assert(failed.responded);
  assert(!failed.blocksOtherRequest(91));

  // At the absolute Wi-Fi deadline, the gate wins over a simultaneous ACK.
  // Disconnect suppresses a response and never creates a false completion.
  LifecycleHarness deadline;
  assert(deadline.accept(72, 720, 299000));
  deadline.pollStart(300000, false, true, true, false);
  assert(deadline.leaseHeld);
  assert(!deadline.completed);
  deadline.finishCleanup(false);
  assert(!deadline.completed);
  assert(!deadline.responded);
  assert(!deadline.leaseHeld);

  return 0;
}
