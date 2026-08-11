#include <assert.h>
#include <initializer_list>

#include "../PokePodAmoled/LinkOperation.h"

using namespace pokepod;

namespace {

void releaseTerminal(LinkOperation &operation, bool expectComplete,
                     bool expectCoordinatorRelease) {
  const LinkOperationSettlement settlement = operation.settlement();
  assert(settlement.ready);
  assert(settlement.rememberCompleted == expectComplete);
  assert(settlement.releaseRequest);
  assert(settlement.releaseCoordinator == expectCoordinatorRelease);
  assert(operation.beginRelease());
  assert(operation.finishRelease());
}

}  // namespace

int main() {
  // Same request frames coalesce; a different request or connection
  // generation cannot steal the sole transport operation.
  LinkOperation upload;
  assert(upload.admit(41, LinkTransport::usb, 7, true) ==
         LinkOperationAdmission::accepted);
  assert(upload.admit(41, LinkTransport::usb, 7, true) ==
         LinkOperationAdmission::sameRequest);
  assert(upload.admit(42, LinkTransport::usb, 7, true) ==
         LinkOperationAdmission::busy);
  assert(upload.admit(41, LinkTransport::usb, 8, true) ==
         LinkOperationAdmission::busy);
  assert(upload.enter(LinkOperationState::receiving));
  upload.ownResource(LinkOperationResource::storageReservation);
  upload.ownResource(LinkOperationResource::file);

  // Every binary_ack is progress. Draining one or many progress frames never
  // creates completion or a coordinator release action.
  assert(upload.queueFrame(LinkOperationFrameRole::progress));
  assert(upload.frameDrained(LinkOperationFrameRole::progress, 7));
  assert(!upload.settlement().ready);
  assert(upload.queueFrame(LinkOperationFrameRole::progress));
  assert(upload.frameDrained(LinkOperationFrameRole::progress, 7));
  assert(!upload.settlement().ready);

  // Durable commit and owner-scoped cleanup may finish before or after the
  // terminal response drains. Both orders converge on one release.
  assert(upload.enter(LinkOperationState::durableCommit));
  upload.ownResource(LinkOperationResource::transaction);
  upload.releaseResource(LinkOperationResource::file);
  assert(upload.enter(LinkOperationState::cleanup));
  assert(upload.queueFrame(LinkOperationFrameRole::terminal, true));
  assert(upload.frameDrained(LinkOperationFrameRole::terminal, 6) == false);
  assert(upload.frameDrained(LinkOperationFrameRole::terminal, 7));
  assert(!upload.settlement().ready);
  upload.releaseResource(LinkOperationResource::transaction);
  upload.releaseResource(LinkOperationResource::storageReservation);
  releaseTerminal(upload, true, true);

  // A terminal response can drain after cleanup. It remains the sole event
  // eligible to complete the request.
  LinkOperation cleanupFirst;
  assert(cleanupFirst.admit(43, LinkTransport::usb, 9, true) ==
         LinkOperationAdmission::accepted);
  assert(cleanupFirst.enter(LinkOperationState::processing));
  cleanupFirst.ownResource(LinkOperationResource::transaction);
  assert(cleanupFirst.enter(LinkOperationState::cleanup));
  cleanupFirst.releaseResource(LinkOperationResource::transaction);
  assert(cleanupFirst.queueFrame(LinkOperationFrameRole::terminal, true));
  assert(!cleanupFirst.settlement().ready);
  assert(cleanupFirst.frameDrained(LinkOperationFrameRole::terminal, 9));
  releaseTerminal(cleanupFirst, true, true);

  // Wi-Fi observes one absolute deadline. At 300 seconds it cancels instead
  // of accepting a simultaneous late ACK; rollback and cleanup then release
  // without a response or completed-request fact.
  LinkOperation deadline;
  assert(deadline.admit(44, LinkTransport::wifi, 10, true, true, 300000) ==
         LinkOperationAdmission::accepted);
  assert(deadline.enter(LinkOperationState::processing));
  deadline.ownResource(LinkOperationResource::file);
  deadline.ownResource(LinkOperationResource::storageReservation);
  assert(deadline.observeDeadline(299999));
  assert(!deadline.observeDeadline(300000));
  assert(deadline.cancelReason() == LinkOperationCancelReason::deadline);
  assert(!deadline.queueFrame(LinkOperationFrameRole::terminal, true));
  assert(deadline.enter(LinkOperationState::rollback));
  deadline.releaseResource(LinkOperationResource::file);
  assert(deadline.enter(LinkOperationState::cleanup));
  deadline.releaseResource(LinkOperationResource::storageReservation);
  releaseTerminal(deadline, false, true);

  // USB has no deadline. Disconnect suppresses any future response, ignores a
  // stale terminal drain after reconnect and releases only after cleanup.
  LinkOperation disconnect;
  assert(disconnect.admit(45, LinkTransport::usb, 11, true) ==
         LinkOperationAdmission::accepted);
  disconnect.ownResource(LinkOperationResource::router);
  assert(disconnect.observeDeadline(0xffffffffU));
  disconnect.cancel(LinkOperationCancelReason::disconnect);
  assert(!disconnect.frameDrained(LinkOperationFrameRole::terminal, 12));
  assert(disconnect.enter(LinkOperationState::cleanup));
  assert(!disconnect.settlement().ready);
  disconnect.releaseResource(LinkOperationResource::router);
  releaseTerminal(disconnect, false, true);

  // Once a real terminal response drained, a later transport disconnect does
  // not erase its completion fact. Cleanup still owns the request until its
  // last resource is released.
  LinkOperation drainedThenDisconnected;
  assert(drainedThenDisconnected.admit(451, LinkTransport::wifi, 111, true,
                                        true, 300000) ==
         LinkOperationAdmission::accepted);
  drainedThenDisconnected.ownResource(LinkOperationResource::file);
  assert(drainedThenDisconnected.enter(LinkOperationState::cleanup));
  assert(drainedThenDisconnected.queueFrame(
      LinkOperationFrameRole::terminal, true));
  assert(drainedThenDisconnected.frameDrained(
      LinkOperationFrameRole::terminal, 111));
  drainedThenDisconnected.cancel(LinkOperationCancelReason::disconnect);
  drainedThenDisconnected.releaseResource(LinkOperationResource::file);
  releaseTerminal(drainedThenDisconnected, true, true);

  // A maintenance request may release its request while retaining the same
  // transport coordinator for the next operation. End-maintenance clears the
  // retain flag and releases it at its own terminal drain.
  LinkOperation maintenance;
  assert(maintenance.admit(46, LinkTransport::usb, 13, true) ==
         LinkOperationAdmission::accepted);
  maintenance.retainCoordinatorAfterTerminal(true);
  assert(maintenance.queueFrame(LinkOperationFrameRole::terminal, true));
  assert(maintenance.frameDrained(LinkOperationFrameRole::terminal, 13));
  releaseTerminal(maintenance, true, false);
  assert(maintenance.ownsResource(LinkOperationResource::coordinator));
  assert(maintenance.admit(47, LinkTransport::usb, 13, false) ==
         LinkOperationAdmission::accepted);
  maintenance.retainCoordinatorAfterTerminal(false);
  assert(maintenance.queueFrame(LinkOperationFrameRole::terminal, true));
  assert(maintenance.frameDrained(LinkOperationFrameRole::terminal, 13));
  releaseTerminal(maintenance, true, true);

  // Enumerate response/cleanup ordering for every resource bit. No terminal
  // settlement may retain a File, reservation, router or transaction owner.
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
      assert(permutation.enter(LinkOperationState::processing));
      if (responseFirst) {
        assert(permutation.enter(LinkOperationState::cleanup));
        assert(permutation.queueFrame(LinkOperationFrameRole::terminal, true));
        assert(permutation.frameDrained(LinkOperationFrameRole::terminal, 21));
        assert(!permutation.settlement().ready);
        permutation.releaseResource(resource);
      } else {
        permutation.releaseResource(resource);
        assert(permutation.enter(LinkOperationState::cleanup));
        assert(permutation.queueFrame(LinkOperationFrameRole::terminal, true));
        assert(permutation.frameDrained(LinkOperationFrameRole::terminal, 21));
      }
      assert(permutation.operationalResourcesDrained());
      releaseTerminal(permutation, true, true);
    }
  }

  return 0;
}
