#include <assert.h>

#include "../PokePodAmoled/LinkOperation.h"
#include "../PokePodAmoled/LinkPolicy.h"

using namespace pokepod;

namespace {

struct AdapterHarness {
  LinkOperation operation;
  bool coordinatorHeld = false;
  bool requestHeld = false;
  bool completed = false;
  unsigned coordinatorReleaseAttempts = 0;
  unsigned requestReleaseAttempts = 0;

  void accept(uint32_t requestId, LinkTransport transport,
              uint32_t generation, bool deadline = false,
              uint32_t deadlineMs = 0) {
    coordinatorHeld = true;
    requestHeld = true;
    assert(operation.admit(requestId, transport, generation, true,
                           deadline, deadlineMs) ==
           LinkOperationAdmission::accepted);
  }

  bool settle(bool externalSuccess = true) {
    if (!operation.settlement().ready || !operation.beginRelease()) {
      return false;
    }
    LinkOperationSettlement pending = operation.settlement();
    if (pending.rememberCompleted) {
      if (externalSuccess) completed = true;
      assert(operation.acknowledgeCompletion(externalSuccess) ==
             externalSuccess);
    }
    pending = operation.settlement();
    if (pending.releaseRequest) {
      ++requestReleaseAttempts;
      if (externalSuccess) requestHeld = false;
      assert(operation.acknowledgeRelease(LinkOperationResource::request,
                                          externalSuccess) ==
             externalSuccess);
    }
    pending = operation.settlement();
    if (pending.releaseCoordinator) {
      ++coordinatorReleaseAttempts;
      if (externalSuccess) coordinatorHeld = false;
      assert(operation.acknowledgeRelease(
                 LinkOperationResource::coordinator, externalSuccess) ==
             externalSuccess);
    }
    return operation.finishRelease();
  }
};

struct ServiceEpochHarness {
  LinkOperation operation;
  LinkRequestHistory history;
  uint32_t liveGeneration = 0;
  uint32_t coordinatorGeneration = 0;

  void connect(uint32_t generation) {
    assert(generation != 0);
    liveGeneration = generation;
    history.clear();
  }

  void disconnect() { liveGeneration = 0; }

  LinkOperationAdmission admit(uint32_t requestId) {
    if (coordinatorGeneration != 0) return LinkOperationAdmission::busy;
    const LinkOperationAdmission result = operation.admit(
        requestId, LinkTransport::wifi, liveGeneration, true);
    if (result == LinkOperationAdmission::accepted) {
      coordinatorGeneration = liveGeneration;
    }
    return result;
  }

  bool settle() {
    if (!operation.settlement().ready || !operation.beginRelease()) {
      return false;
    }
    const uint32_t requestId = operation.requestId();
    const bool current = liveGeneration != 0 &&
        operation.connectionGeneration() == liveGeneration;
    const bool completed = !current || requestId == 0 ||
        history.contains(requestId) ||
        history.complete(requestId);
    if (operation.settlement().rememberCompleted) {
      assert(operation.acknowledgeCompletion(completed));
    }
    if (operation.settlement().releaseRequest) {
      assert(operation.acknowledgeRelease(
          LinkOperationResource::request, true));
    }
    if (operation.settlement().releaseCoordinator) {
      if (coordinatorGeneration == operation.connectionGeneration()) {
        coordinatorGeneration = 0;
      }
      assert(operation.acknowledgeRelease(
          LinkOperationResource::coordinator,
          coordinatorGeneration == 0));
    }
    return operation.finishRelease();
  }
};

struct TerminalFallbackHarness {
  LinkOperation operation;
  bool connected = true;
  bool fallbackQueued = false;

  void accept(uint32_t requestId, uint32_t generation) {
    assert(operation.admit(requestId, LinkTransport::wifi, generation, true) ==
           LinkOperationAdmission::accepted);
  }

  bool send(size_t primaryBytes, bool primaryQueueAvailable,
            bool fallbackQueueAvailable, uint32_t generation) {
    constexpr size_t maximum = 4096;
    if (primaryBytes <= maximum && primaryQueueAvailable) {
      return operation.queueFrame(LinkOperationFrameRole::terminal,
                                  generation, 1, true);
    }
    if (fallbackQueueAvailable) {
      fallbackQueued = operation.queueFrame(
          LinkOperationFrameRole::terminal, generation, 2, false);
      return false;
    }
    operation.cancel(LinkOperationCancelReason::operationFailure);
    operation.discardQueuedFrames(generation);
    connected = false;
    return false;
  }
};

void releaseOperationalResources(LinkOperation &operation) {
  operation.releaseResource(LinkOperationResource::file);
  operation.releaseResource(LinkOperationResource::storageReservation);
  operation.releaseResource(LinkOperationResource::transaction);
  operation.releaseResource(LinkOperationResource::router);
}

}  // namespace

int main() {
  // A binary upload can emit any number of progress ACKs. Draining the first
  // ACK never settles request/coordinator ownership or records completion.
  AdapterHarness upload;
  upload.accept(100, LinkTransport::wifi, 5, true, 300000);
  assert(upload.operation.advance(LinkOperationState::receiving));
  upload.operation.ownResource(LinkOperationResource::file);
  upload.operation.ownResource(LinkOperationResource::storageReservation);
  assert(upload.operation.queueFrame(LinkOperationFrameRole::progress, 5,
                                     1000));
  assert(upload.operation.frameDrained(LinkOperationFrameRole::progress, 5,
                                       1001));
  assert(!upload.operation.settlement().ready);
  assert(upload.coordinatorHeld && upload.requestHeld && !upload.completed);
  assert(upload.operation.advance(LinkOperationState::durableCommit));
  upload.operation.ownResource(LinkOperationResource::transaction);
  upload.operation.releaseResource(LinkOperationResource::file);
  assert(upload.operation.advance(LinkOperationState::cleanup));
  assert(upload.operation.queueFrame(LinkOperationFrameRole::terminal, 5,
                                     2000, true));
  assert(upload.operation.frameDrained(LinkOperationFrameRole::terminal, 5,
                                       2001));
  assert(!upload.operation.settlement().ready);
  upload.operation.releaseResource(LinkOperationResource::transaction);
  upload.operation.releaseResource(LinkOperationResource::storageReservation);
  assert(upload.settle());
  assert(upload.completed && !upload.requestHeld && !upload.coordinatorHeld);

  // If the transport cannot queue a terminal frame, cancellation rolls back
  // every counted progress/data frame before external ownership can settle.
  AdapterHarness queueFailure;
  queueFailure.accept(101, LinkTransport::usb, 6);
  assert(queueFailure.operation.advance(LinkOperationState::processing));
  queueFailure.operation.ownResource(LinkOperationResource::file);
  assert(queueFailure.operation.queueFrame(LinkOperationFrameRole::data, 6,
                                           0));
  queueFailure.operation.cancel(LinkOperationCancelReason::operationFailure);
  assert(!queueFailure.operation.settlement().ready);
  assert(queueFailure.operation.discardQueuedFrames(6));
  assert(queueFailure.operation.advance(LinkOperationState::rollback));
  queueFailure.operation.releaseResource(LinkOperationResource::file);
  assert(queueFailure.operation.advance(LinkOperationState::cleanup));
  assert(queueFailure.settle());
  assert(!queueFailure.completed);

  // A write failure/disconnect drops queued bytes immediately, while local
  // owner-scoped cleanup remains the only thing delaying settlement.
  AdapterHarness writeFailure;
  writeFailure.accept(102, LinkTransport::wifi, 7, true, 300000);
  assert(writeFailure.operation.advance(LinkOperationState::processing));
  writeFailure.operation.ownResource(LinkOperationResource::file);
  writeFailure.operation.ownResource(
      LinkOperationResource::storageReservation);
  assert(writeFailure.operation.queueFrame(LinkOperationFrameRole::data, 7,
                                           299000));
  assert(writeFailure.operation.queueFrame(LinkOperationFrameRole::terminal,
                                           7, 299001, true));
  writeFailure.operation.cancel(LinkOperationCancelReason::disconnect);
  assert(writeFailure.operation.discardQueuedFrames(7));
  assert(writeFailure.operation.advance(LinkOperationState::rollback));
  releaseOperationalResources(writeFailure.operation);
  assert(writeFailure.operation.advance(LinkOperationState::cleanup));
  assert(writeFailure.settle());
  assert(!writeFailure.completed);

  // The exact wireless cutoff wins over a final transport drain. Cleanup can
  // continue, but it cannot produce a completed request.
  AdapterHarness deadline;
  deadline.accept(103, LinkTransport::wifi, 8, true, 300000);
  assert(deadline.operation.advance(LinkOperationState::processing));
  deadline.operation.ownResource(LinkOperationResource::transaction);
  assert(deadline.operation.queueFrame(LinkOperationFrameRole::terminal, 8,
                                       299999, true));
  assert(!deadline.operation.frameDrained(LinkOperationFrameRole::terminal,
                                          8, 300000));
  assert(deadline.operation.discardQueuedFrames(8));
  assert(deadline.operation.advance(LinkOperationState::rollback));
  deadline.operation.releaseResource(LinkOperationResource::transaction);
  assert(deadline.operation.advance(LinkOperationState::cleanup));
  assert(deadline.settle());
  assert(!deadline.completed);

  // Settlement actions are retried explicitly. A failed external release does
  // not clear the model's logical ownership.
  AdapterHarness retry;
  retry.accept(104, LinkTransport::usb, 9);
  assert(retry.operation.queueFrame(LinkOperationFrameRole::terminal, 9, 0,
                                    true));
  assert(retry.operation.frameDrained(LinkOperationFrameRole::terminal, 9,
                                      0));
  assert(!retry.settle(false));
  assert(retry.operation.ownsResource(LinkOperationResource::request));
  assert(retry.operation.ownsResource(LinkOperationResource::coordinator));
  assert(retry.requestHeld && retry.coordinatorHeld && !retry.completed);
  assert(retry.settle(true));
  assert(retry.requestReleaseAttempts == 2);
  assert(retry.coordinatorReleaseAttempts == 2);
  assert(retry.completed && !retry.requestHeld && !retry.coordinatorHeld);

  // Maintenance completion retains the exact connection generation. An idle
  // disconnect/quiesce creates one request-less settlement and releases it.
  AdapterHarness maintenance;
  maintenance.accept(105, LinkTransport::usb, 10);
  maintenance.operation.retainCoordinatorAfterTerminal(true);
  assert(maintenance.operation.queueFrame(LinkOperationFrameRole::terminal,
                                          10, 0, true));
  assert(maintenance.operation.frameDrained(LinkOperationFrameRole::terminal,
                                            10, 0));
  assert(maintenance.settle());
  assert(!maintenance.requestHeld && maintenance.coordinatorHeld);
  assert(maintenance.operation.admit(106, LinkTransport::usb, 11, false) ==
         LinkOperationAdmission::busy);
  assert(maintenance.operation.cancelRetainedCoordinator(
      LinkTransport::usb, 10, LinkOperationCancelReason::quiesce));
  assert(maintenance.settle());
  assert(!maintenance.coordinatorHeld);
  assert(maintenance.coordinatorReleaseAttempts == 1);

  // A disconnected operation may finish deferred cleanup after a new
  // physical connection uses the same request id.  Its old epoch is
  // acknowledged locally and never inserts into the new replay cache.
  ServiceEpochHarness epoch;
  epoch.connect(20);
  assert(epoch.admit(200) == LinkOperationAdmission::accepted);
  epoch.operation.ownResource(LinkOperationResource::file);
  epoch.operation.cancel(LinkOperationCancelReason::disconnect);
  epoch.disconnect();
  epoch.connect(21);
  assert(!epoch.history.contains(200));
  assert(epoch.admit(200) == LinkOperationAdmission::busy);
  assert(epoch.coordinatorGeneration == 20);
  assert(epoch.operation.advance(LinkOperationState::cleanup));
  epoch.operation.releaseResource(LinkOperationResource::file);
  assert(epoch.settle());
  assert(!epoch.history.contains(200));
  assert(epoch.coordinatorGeneration == 0);
  assert(epoch.admit(200) == LinkOperationAdmission::accepted);

  // Oversized dynamic status never leaves a live client waiting. The primary
  // payload can fall back to one fixed terminal error; if even that cannot be
  // queued, the connection is cancelled and closed.
  TerminalFallbackHarness fallback;
  fallback.accept(300, 30);
  assert(!fallback.send(5000, true, true, 30));
  assert(fallback.connected && fallback.fallbackQueued);
  assert(fallback.operation.frameDrained(LinkOperationFrameRole::terminal,
                                         30, 3));

  TerminalFallbackHarness noFallback;
  noFallback.accept(301, 31);
  assert(!noFallback.send(5000, true, false, 31));
  assert(!noFallback.connected);
  assert(noFallback.operation.cancelled());

  TerminalFallbackHarness primaryQueueFailure;
  primaryQueueFailure.accept(302, 32);
  assert(!primaryQueueFailure.send(1000, false, true, 32));
  assert(primaryQueueFailure.connected &&
         primaryQueueFailure.fallbackQueued);

  // IO-busy read/result is a retryable terminal response.  It releases only
  // after that response drains, rather than leaving an accepted request idle.
  AdapterHarness ioBusyRead;
  ioBusyRead.accept(201, LinkTransport::wifi, 22, true, 300000);
  assert(ioBusyRead.operation.queueFrame(LinkOperationFrameRole::terminal, 22,
                                         100, false));
  assert(ioBusyRead.operation.frameDrained(LinkOperationFrameRole::terminal,
                                           22, 101));
  assert(ioBusyRead.settle());
  assert(!ioBusyRead.completed);

  return 0;
}
