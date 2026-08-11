#include <assert.h>
#include <initializer_list>

#include "../PokePodAmoled/LinkOperation.h"

using namespace pokepod;

namespace {

void acknowledgeSettlement(LinkOperation &operation, bool expectComplete,
                           bool expectCoordinatorRelease,
                           bool expectRequestRelease = true) {
  LinkOperationSettlement value = operation.settlement();
  assert(value.ready);
  assert(value.rememberCompleted == expectComplete);
  assert(value.releaseRequest == expectRequestRelease);
  assert(value.releaseCoordinator == expectCoordinatorRelease);
  assert(operation.beginRelease());

  // Settlement actions remain pending until the adapter confirms success.
  value = operation.settlement();
  assert(value.ready);
  assert(value.rememberCompleted == expectComplete);
  assert(value.releaseRequest == expectRequestRelease);
  assert(value.releaseCoordinator == expectCoordinatorRelease);
  assert(!operation.finishRelease());

  if (expectComplete) {
    assert(!operation.acknowledgeCompletion(false));
    assert(operation.settlement().rememberCompleted);
    assert(operation.acknowledgeCompletion(true));
    assert(!operation.settlement().rememberCompleted);
    assert(!operation.acknowledgeCompletion(true));
  }
  if (expectRequestRelease) {
    assert(!operation.acknowledgeRelease(LinkOperationResource::request,
                                         false));
    assert(operation.settlement().releaseRequest);
    assert(operation.acknowledgeRelease(LinkOperationResource::request,
                                        true));
    assert(!operation.settlement().releaseRequest);
    assert(!operation.acknowledgeRelease(LinkOperationResource::request,
                                         true));
  }
  if (expectCoordinatorRelease) {
    assert(!operation.acknowledgeRelease(LinkOperationResource::coordinator,
                                         false));
    assert(operation.settlement().releaseCoordinator);
    assert(operation.acknowledgeRelease(LinkOperationResource::coordinator,
                                        true));
    assert(!operation.settlement().releaseCoordinator);
  }
  assert(operation.finishRelease());
}

void drainTerminal(LinkOperation &operation, uint32_t generation,
                   uint32_t nowMs, bool complete = true) {
  assert(operation.queueFrame(LinkOperationFrameRole::terminal, generation,
                              nowMs, complete));
  assert(operation.frameDrained(LinkOperationFrameRole::terminal, generation,
                                nowMs));
}

}  // namespace

int main() {
  // Public generic transitions expose only ordinary work phases. Every
  // lifecycle-special state is rejected and must use its dedicated method.
  const LinkOperationState specialStates[] = {
      LinkOperationState::idle,
      LinkOperationState::accepted,
      LinkOperationState::terminalResponse,
      LinkOperationState::responseDrained,
      LinkOperationState::cancelled,
      LinkOperationState::blocked,
      LinkOperationState::release,
  };
  for (LinkOperationState special : specialStates) {
    LinkOperation transition;
    assert(transition.admit(1, LinkTransport::usb, 1, true) ==
           LinkOperationAdmission::accepted);
    assert(!transition.advance(special));
  }

  // Enumerate the legal forward work graph and its important forbidden edges.
  LinkOperation phaseGraph;
  assert(phaseGraph.admit(2, LinkTransport::usb, 2, true) ==
         LinkOperationAdmission::accepted);
  assert(phaseGraph.advance(LinkOperationState::receiving));
  assert(phaseGraph.advance(LinkOperationState::processing));
  assert(!phaseGraph.advance(LinkOperationState::receiving));
  assert(phaseGraph.advance(LinkOperationState::durableCommit));
  assert(!phaseGraph.advance(LinkOperationState::processing));
  assert(phaseGraph.advance(LinkOperationState::rollback));
  assert(!phaseGraph.advance(LinkOperationState::durableCommit));
  assert(phaseGraph.advance(LinkOperationState::cleanup));
  assert(!phaseGraph.advance(LinkOperationState::rollback));

  // Admission rejects invalid generations. The same request coalesces only
  // inside the exact transport epoch.
  LinkOperation upload;
  assert(upload.admit(41, LinkTransport::usb, 0, true) ==
         LinkOperationAdmission::invalid);
  assert(upload.admit(41, LinkTransport::usb, 7, true) ==
         LinkOperationAdmission::accepted);
  assert(upload.admit(41, LinkTransport::usb, 7, true) ==
         LinkOperationAdmission::sameRequest);
  assert(upload.admit(42, LinkTransport::usb, 7, true) ==
         LinkOperationAdmission::busy);
  assert(upload.admit(41, LinkTransport::usb, 8, true) ==
         LinkOperationAdmission::busy);
  assert(upload.advance(LinkOperationState::receiving));
  assert(!upload.advance(LinkOperationState::cancelled));
  assert(!upload.advance(LinkOperationState::terminalResponse));
  assert(!upload.advance(LinkOperationState::release));
  upload.ownResource(LinkOperationResource::storageReservation);
  upload.ownResource(LinkOperationResource::file);

  // Progress/data frames are transport-drain resources. A wrong generation
  // cannot queue or drain them, and no progress drain completes the request.
  assert(!upload.queueFrame(LinkOperationFrameRole::progress, 8, 0));
  assert(upload.queueFrame(LinkOperationFrameRole::progress, 7, 0));
  assert(upload.queueFrame(LinkOperationFrameRole::data, 7, 0));
  assert(upload.queuedFrameCount() == 2);
  assert(!upload.frameDrained(LinkOperationFrameRole::progress, 8, 0));
  assert(upload.frameDrained(LinkOperationFrameRole::progress, 7, 0));
  assert(upload.frameDrained(LinkOperationFrameRole::data, 7, 0));
  assert(!upload.settlement().ready);

  // Terminal drain may arrive before the final progress drain. Settlement is
  // withheld until every queued frame has drained; terminal forbids new data.
  assert(upload.advance(LinkOperationState::durableCommit));
  upload.ownResource(LinkOperationResource::transaction);
  upload.releaseResource(LinkOperationResource::file);
  assert(upload.advance(LinkOperationState::cleanup));
  assert(upload.queueFrame(LinkOperationFrameRole::progress, 7, 1));
  assert(upload.queueFrame(LinkOperationFrameRole::terminal, 7, 1, true));
  assert(!upload.queueFrame(LinkOperationFrameRole::data, 7, 1));
  assert(!upload.frameDrained(LinkOperationFrameRole::terminal, 6, 1));
  assert(upload.frameDrained(LinkOperationFrameRole::terminal, 7, 1));
  assert(upload.state() == LinkOperationState::terminalResponse);
  upload.releaseResource(LinkOperationResource::transaction);
  upload.releaseResource(LinkOperationResource::storageReservation);
  assert(!upload.settlement().ready);
  assert(upload.frameDrained(LinkOperationFrameRole::progress, 7, 1));
  assert(upload.state() == LinkOperationState::responseDrained);
  acknowledgeSettlement(upload, true, true);

  // A terminal response can drain after cleanup. It remains the sole event
  // eligible to complete the request.
  LinkOperation cleanupFirst;
  assert(cleanupFirst.admit(43, LinkTransport::usb, 9, true) ==
         LinkOperationAdmission::accepted);
  assert(cleanupFirst.advance(LinkOperationState::processing));
  cleanupFirst.ownResource(LinkOperationResource::transaction);
  assert(cleanupFirst.advance(LinkOperationState::cleanup));
  cleanupFirst.releaseResource(LinkOperationResource::transaction);
  drainTerminal(cleanupFirst, 9, 10);
  acknowledgeSettlement(cleanupFirst, true, true);

  // At the exact absolute deadline, terminal drain loses the race: completion
  // is suppressed and queued frames must be discarded by the cancelled socket.
  LinkOperation deadline;
  assert(deadline.admit(44, LinkTransport::wifi, 10, true, true, 300000) ==
         LinkOperationAdmission::accepted);
  assert(deadline.advance(LinkOperationState::processing));
  deadline.ownResource(LinkOperationResource::file);
  deadline.ownResource(LinkOperationResource::storageReservation);
  assert(deadline.queueFrame(LinkOperationFrameRole::terminal, 10, 299999,
                             true));
  assert(!deadline.frameDrained(LinkOperationFrameRole::terminal, 10,
                                300000));
  assert(deadline.cancelReason() == LinkOperationCancelReason::deadline);
  assert(!deadline.settlement().ready);
  assert(deadline.discardQueuedFrames(10));
  assert(deadline.advance(LinkOperationState::rollback));
  deadline.releaseResource(LinkOperationResource::file);
  assert(deadline.advance(LinkOperationState::cleanup));
  deadline.releaseResource(LinkOperationResource::storageReservation);
  acknowledgeSettlement(deadline, false, true);

  // Disconnect suppresses responses and stale frame drains. Local rollback may
  // continue, but settlement waits for the adapter to discard transport frames.
  LinkOperation disconnect;
  assert(disconnect.admit(45, LinkTransport::usb, 11, true) ==
         LinkOperationAdmission::accepted);
  disconnect.ownResource(LinkOperationResource::router);
  assert(disconnect.queueFrame(LinkOperationFrameRole::progress, 11, 0));
  disconnect.cancel(LinkOperationCancelReason::disconnect);
  assert(!disconnect.frameDrained(LinkOperationFrameRole::progress, 12, 0));
  assert(disconnect.advance(LinkOperationState::cleanup));
  disconnect.releaseResource(LinkOperationResource::router);
  assert(!disconnect.settlement().ready);
  assert(disconnect.discardQueuedFrames(11));
  acknowledgeSettlement(disconnect, false, true);

  // Once a real terminal response drained, a later disconnect preserves the
  // completion fact while owner-scoped cleanup still runs.
  LinkOperation drainedThenDisconnected;
  assert(drainedThenDisconnected.admit(451, LinkTransport::wifi, 111, true,
                                        true, 300000) ==
         LinkOperationAdmission::accepted);
  drainedThenDisconnected.ownResource(LinkOperationResource::file);
  assert(drainedThenDisconnected.advance(LinkOperationState::cleanup));
  drainTerminal(drainedThenDisconnected, 111, 20);
  drainedThenDisconnected.cancel(LinkOperationCancelReason::disconnect);
  drainedThenDisconnected.releaseResource(LinkOperationResource::file);
  acknowledgeSettlement(drainedThenDisconnected, true, true);

  // A retained maintenance coordinator preserves its exact transport epoch.
  // A new generation cannot steal it; the same epoch can continue.
  LinkOperation maintenance;
  assert(maintenance.admit(46, LinkTransport::usb, 13, true) ==
         LinkOperationAdmission::accepted);
  maintenance.retainCoordinatorAfterTerminal(true);
  drainTerminal(maintenance, 13, 0);
  acknowledgeSettlement(maintenance, true, false);
  assert(maintenance.ownsResource(LinkOperationResource::coordinator));
  assert(maintenance.transport() == LinkTransport::usb);
  assert(maintenance.connectionGeneration() == 13);
  assert(maintenance.admit(47, LinkTransport::usb, 14, false) ==
         LinkOperationAdmission::busy);
  assert(maintenance.admit(47, LinkTransport::wifi, 13, false) ==
         LinkOperationAdmission::busy);
  assert(maintenance.admit(47, LinkTransport::usb, 13, false) ==
         LinkOperationAdmission::accepted);
  maintenance.retainCoordinatorAfterTerminal(false);
  drainTerminal(maintenance, 13, 0);
  acknowledgeSettlement(maintenance, true, true);

  // If the retained connection disappears while idle, its coordinator has one
  // explicit settlement. A stale or repeated generation cannot settle it.
  LinkOperation retainedDisconnect;
  assert(retainedDisconnect.admit(48, LinkTransport::wifi, 15, true) ==
         LinkOperationAdmission::accepted);
  retainedDisconnect.retainCoordinatorAfterTerminal(true);
  drainTerminal(retainedDisconnect, 15, 1);
  acknowledgeSettlement(retainedDisconnect, true, false);
  assert(!retainedDisconnect.cancelRetainedCoordinator(
      LinkTransport::wifi, 16, LinkOperationCancelReason::disconnect));
  assert(retainedDisconnect.cancelRetainedCoordinator(
      LinkTransport::wifi, 15, LinkOperationCancelReason::disconnect));
  assert(!retainedDisconnect.cancelRetainedCoordinator(
      LinkTransport::wifi, 15, LinkOperationCancelReason::disconnect));
  acknowledgeSettlement(retainedDisconnect, false, true, false);
  assert(retainedDisconnect.transport() == LinkTransport::none);
  assert(retainedDisconnect.connectionGeneration() == 0);

  // Enumerate response/cleanup ordering for every non-transport resource. No
  // terminal settlement may retain a File, reservation, router or transaction.
  const LinkOperationResource resources[] = {
      LinkOperationResource::storageReservation,
      LinkOperationResource::file,
      LinkOperationResource::router,
      LinkOperationResource::transaction,
  };
  uint32_t request = 100;
  for (LinkOperationResource resource : resources) {
    for (bool responseFirst : {false, true}) {
      LinkOperation permutation;
      assert(permutation.admit(request++, LinkTransport::wifi, 21, true,
                               true, 1000) ==
             LinkOperationAdmission::accepted);
      permutation.ownResource(resource);
      assert(permutation.advance(LinkOperationState::processing));
      if (responseFirst) {
        assert(permutation.advance(LinkOperationState::cleanup));
        drainTerminal(permutation, 21, 999);
        assert(!permutation.settlement().ready);
        permutation.releaseResource(resource);
      } else {
        permutation.releaseResource(resource);
        assert(permutation.advance(LinkOperationState::cleanup));
        drainTerminal(permutation, 21, 999);
      }
      assert(permutation.operationalResourcesDrained());
      acknowledgeSettlement(permutation, true, true);
    }
  }

  // Blocked cleanup never emits a response/completion but still requires the
  // same externally acknowledged release sequence.
  LinkOperation blocked;
  assert(blocked.admit(999, LinkTransport::usb, 50, true) ==
         LinkOperationAdmission::accepted);
  blocked.ownResource(LinkOperationResource::file);
  assert(blocked.block(LinkOperationCancelReason::operationFailure));
  assert(!blocked.queueFrame(LinkOperationFrameRole::terminal, 50, 0, true));
  blocked.releaseResource(LinkOperationResource::file);
  acknowledgeSettlement(blocked, false, true);

  return 0;
}
