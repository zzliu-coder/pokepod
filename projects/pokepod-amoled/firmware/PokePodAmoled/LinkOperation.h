#pragma once

#include <stdint.h>

#include "LinkServiceCoordinator.h"

namespace pokepod {

enum class LinkOperationState : uint8_t {
  idle,
  accepted,
  receiving,
  processing,
  durableCommit,
  cleanup,
  terminalResponse,
  responseDrained,
  cancelled,
  rollback,
  blocked,
  release,
};

enum class LinkOperationAdmission : uint8_t {
  accepted,
  sameRequest,
  busy,
  invalid,
};

enum class LinkOperationFrameRole : uint8_t {
  progress,
  terminal,
  data,
};

enum class LinkOperationCancelReason : uint8_t {
  none,
  disconnect,
  deadline,
  quiesce,
  operationFailure,
};

enum class LinkOperationResource : uint16_t {
  request = 1U << 0,
  coordinator = 1U << 1,
  storageReservation = 1U << 2,
  file = 1U << 3,
  router = 1U << 4,
  transaction = 1U << 5,
};

constexpr uint16_t linkOperationResourceBit(LinkOperationResource resource) {
  return static_cast<uint16_t>(resource);
}

struct LinkOperationSettlement {
  bool ready = false;
  bool rememberCompleted = false;
  bool releaseRequest = false;
  bool releaseCoordinator = false;
};

// Transport-independent request lifecycle. It owns no socket, File or Arduino
// object. The production adapter publishes resource facts, executes settlement
// actions and acknowledges each successful external action. Queueing a frame
// never releases ownership or records a completed request.
class LinkOperation {
 public:
  LinkOperationAdmission admit(uint32_t requestId, LinkTransport transport,
                               uint32_t connectionGeneration,
                               bool coordinatorOwned,
                               bool deadlineArmed = false,
                               uint32_t absoluteDeadlineMs = 0) {
    if (requestId == 0 || connectionGeneration == 0 ||
        transport == LinkTransport::none) {
      return LinkOperationAdmission::invalid;
    }
    if (requestActive()) {
      return owns(requestId, transport, connectionGeneration)
          ? LinkOperationAdmission::sameRequest
          : LinkOperationAdmission::busy;
    }
    if (state_ != LinkOperationState::idle) {
      return LinkOperationAdmission::busy;
    }
    if (ownsResource(LinkOperationResource::coordinator) &&
        (transport_ != transport ||
         connectionGeneration_ != connectionGeneration)) {
      return LinkOperationAdmission::busy;
    }
    if (!ownsResource(LinkOperationResource::coordinator)) {
      transport_ = transport;
      connectionGeneration_ = connectionGeneration;
    }
    requestId_ = requestId;
    deadlineArmed_ = deadlineArmed;
    absoluteDeadlineMs_ = absoluteDeadlineMs;
    cancelReason_ = LinkOperationCancelReason::none;
    responseAllowed_ = true;
    responseDrained_ = false;
    completionEligible_ = false;
    cancelled_ = false;
    blocked_ = false;
    terminalFrameQueued_ = false;
    progressFramesQueued_ = 0;
    dataFramesQueued_ = 0;
    clearReleaseAcknowledgements();
    resources_ |= linkOperationResourceBit(LinkOperationResource::request);
    if (coordinatorOwned) {
      resources_ |= linkOperationResourceBit(
          LinkOperationResource::coordinator);
    }
    state_ = LinkOperationState::accepted;
    return LinkOperationAdmission::accepted;
  }

  bool active() const { return requestActive(); }

  bool owns(uint32_t requestId, LinkTransport transport,
            uint32_t connectionGeneration) const {
    return requestActive() && requestId_ == requestId &&
        transport_ == transport &&
        connectionGeneration_ == connectionGeneration;
  }

  // Only ordinary work phases are reachable through the generic transition.
  // Cancellation, blocking, terminal response/drain and release each have a
  // dedicated invariant-preserving method below.
  bool advance(LinkOperationState next) {
    if (!ordinaryWorkState(next) || !allowedWorkTransition(state_, next)) {
      return false;
    }
    state_ = next;
    return true;
  }

  bool observeDeadline(uint32_t nowMs) {
    if (!requestActive()) return false;
    if (!deadlineExpired(nowMs) || responseDrained_) return true;
    cancel(LinkOperationCancelReason::deadline);
    return false;
  }

  void cancel(LinkOperationCancelReason reason) {
    if (!requestActive() || state_ == LinkOperationState::release) return;
    cancelReason_ = reason;
    cancelled_ = true;
    responseAllowed_ = false;
    if (!responseDrained_) completionEligible_ = false;
    state_ = LinkOperationState::cancelled;
  }

  bool block(LinkOperationCancelReason reason) {
    if (!requestActive() || state_ == LinkOperationState::release) return false;
    cancelReason_ = reason;
    blocked_ = true;
    responseAllowed_ = false;
    if (!responseDrained_) completionEligible_ = false;
    state_ = LinkOperationState::blocked;
    return true;
  }

  // A retained maintenance coordinator exists while no request is active.
  // Only the exact transport epoch that owns it may settle its disconnect.
  bool cancelRetainedCoordinator(LinkTransport transport,
                                 uint32_t connectionGeneration,
                                 LinkOperationCancelReason reason) {
    if (state_ != LinkOperationState::idle || requestId_ != 0 ||
        !ownsResource(LinkOperationResource::coordinator) ||
        transport_ != transport ||
        connectionGeneration_ != connectionGeneration) {
      return false;
    }
    retainCoordinator_ = false;
    cancelReason_ = reason;
    cancelled_ = true;
    responseAllowed_ = false;
    state_ = LinkOperationState::cancelled;
    return true;
  }

  bool queueFrame(LinkOperationFrameRole role,
                  uint32_t connectionGeneration, uint32_t nowMs,
                  bool completionEligible = false) {
    if (!requestActive() || !responseAllowed_ || connectionGeneration == 0 ||
        connectionGeneration != connectionGeneration_ ||
        !observeDeadline(nowMs)) {
      return false;
    }
    if (role == LinkOperationFrameRole::terminal) {
      if (!terminalAllowed(state_) || terminalFrameQueued_ ||
          responseDrained_) {
        return false;
      }
      terminalFrameQueued_ = true;
      completionEligible_ = completionEligible;
      state_ = LinkOperationState::terminalResponse;
      return true;
    }
    if (terminalFrameQueued_ || responseDrained_ ||
        !nonterminalFrameAllowed(state_)) {
      return false;
    }
    uint16_t &queued = role == LinkOperationFrameRole::progress
        ? progressFramesQueued_
        : dataFramesQueued_;
    if (queued == UINT16_MAX) return false;
    ++queued;
    return true;
  }

  bool frameDrained(LinkOperationFrameRole role,
                    uint32_t connectionGeneration, uint32_t nowMs) {
    if (!requestActive() || connectionGeneration == 0 ||
        connectionGeneration != connectionGeneration_ ||
        !observeDeadline(nowMs)) {
      return false;
    }
    if (role == LinkOperationFrameRole::terminal) {
      if (!terminalFrameQueued_ ||
          state_ != LinkOperationState::terminalResponse) {
        return false;
      }
      terminalFrameQueued_ = false;
      responseDrained_ = true;
      refreshResponseDrainedState();
      return true;
    }
    uint16_t &queued = role == LinkOperationFrameRole::progress
        ? progressFramesQueued_
        : dataFramesQueued_;
    if (queued == 0) return false;
    --queued;
    refreshResponseDrainedState();
    return true;
  }

  // The adapter calls this only after the transport has discarded its queued
  // frames (for example after closing a cancelled socket).
  bool discardQueuedFrames(uint32_t connectionGeneration) {
    if (connectionGeneration == 0 ||
        connectionGeneration != connectionGeneration_ ||
        (!cancelled_ && !blocked_)) {
      return false;
    }
    progressFramesQueued_ = 0;
    dataFramesQueued_ = 0;
    terminalFrameQueued_ = false;
    if (!responseDrained_) completionEligible_ = false;
    return true;
  }

  void ownResource(LinkOperationResource resource) {
    resources_ |= linkOperationResourceBit(resource);
  }

  void releaseResource(LinkOperationResource resource) {
    resources_ &= ~linkOperationResourceBit(resource);
  }

  bool ownsResource(LinkOperationResource resource) const {
    return (resources_ & linkOperationResourceBit(resource)) != 0;
  }

  bool operationalResourcesDrained() const {
    constexpr uint16_t lifecycleResources =
        linkOperationResourceBit(LinkOperationResource::storageReservation) |
        linkOperationResourceBit(LinkOperationResource::file) |
        linkOperationResourceBit(LinkOperationResource::router) |
        linkOperationResourceBit(LinkOperationResource::transaction);
    return (resources_ & lifecycleResources) == 0;
  }

  void retainCoordinatorAfterTerminal(bool retain) {
    retainCoordinator_ = retain;
  }

  LinkOperationSettlement settlement() const {
    LinkOperationSettlement value;
    if (state_ == LinkOperationState::release) {
      value.ready = true;
      value.rememberCompleted = completionAckPending_;
      value.releaseRequest = requestReleaseAckPending_;
      value.releaseCoordinator = coordinatorReleaseAckPending_;
      return value;
    }
    const bool terminal = responseDrained_;
    const bool cancelled = cancelled_ || blocked_;
    if ((!terminal && !cancelled) || !allFramesDrained() ||
        !operationalResourcesDrained()) {
      return value;
    }
    value.ready = true;
    value.rememberCompleted = terminal && completionEligible_;
    value.releaseRequest = ownsResource(LinkOperationResource::request);
    value.releaseCoordinator =
        ownsResource(LinkOperationResource::coordinator) &&
        !retainCoordinator_;
    return value;
  }

  // Starts settlement without claiming that any external action succeeded.
  // The adapter must acknowledge each requested action below; a failed action
  // remains pending and is returned by settlement() for retry.
  bool beginRelease() {
    if (state_ == LinkOperationState::release) return true;
    const LinkOperationSettlement value = settlement();
    if (!value.ready) return false;
    completionAckPending_ = value.rememberCompleted;
    requestReleaseAckPending_ = value.releaseRequest;
    coordinatorReleaseAckPending_ = value.releaseCoordinator;
    state_ = LinkOperationState::release;
    return true;
  }

  bool acknowledgeCompletion(bool success) {
    if (state_ != LinkOperationState::release || !completionAckPending_) {
      return false;
    }
    if (success) completionAckPending_ = false;
    return success;
  }

  bool acknowledgeRelease(LinkOperationResource resource, bool success) {
    if (state_ != LinkOperationState::release ||
        (resource != LinkOperationResource::request &&
         resource != LinkOperationResource::coordinator)) {
      return false;
    }
    bool *pending = resource == LinkOperationResource::request
        ? &requestReleaseAckPending_
        : &coordinatorReleaseAckPending_;
    if (!*pending) return false;
    if (!success) return false;
    *pending = false;
    releaseResource(resource);
    return true;
  }

  bool finishRelease() {
    if (state_ != LinkOperationState::release || completionAckPending_ ||
        requestReleaseAckPending_ || coordinatorReleaseAckPending_ ||
        ownsResource(LinkOperationResource::request) ||
        !operationalResourcesDrained()) {
      return false;
    }
    requestId_ = 0;
    deadlineArmed_ = false;
    absoluteDeadlineMs_ = 0;
    cancelReason_ = LinkOperationCancelReason::none;
    responseAllowed_ = true;
    responseDrained_ = false;
    completionEligible_ = false;
    cancelled_ = false;
    blocked_ = false;
    terminalFrameQueued_ = false;
    progressFramesQueued_ = 0;
    dataFramesQueued_ = 0;
    clearReleaseAcknowledgements();
    if (!ownsResource(LinkOperationResource::coordinator)) {
      transport_ = LinkTransport::none;
      connectionGeneration_ = 0;
      retainCoordinator_ = false;
    }
    state_ = LinkOperationState::idle;
    return true;
  }

  LinkOperationState state() const { return state_; }
  LinkOperationCancelReason cancelReason() const { return cancelReason_; }
  uint32_t requestId() const { return requestId_; }
  LinkTransport transport() const { return transport_; }
  uint32_t connectionGeneration() const { return connectionGeneration_; }
  bool responseAllowed() const { return responseAllowed_; }
  bool responseDrained() const { return responseDrained_; }
  bool cancelled() const { return cancelled_; }
  uint16_t queuedFrameCount() const {
    return progressFramesQueued_ + dataFramesQueued_ +
        (terminalFrameQueued_ ? 1U : 0U);
  }

 private:
  bool requestActive() const {
    return requestId_ != 0 && state_ != LinkOperationState::idle;
  }

  bool deadlineExpired(uint32_t nowMs) const {
    return deadlineArmed_ &&
        static_cast<int32_t>(nowMs - absoluteDeadlineMs_) >= 0;
  }

  bool allFramesDrained() const { return queuedFrameCount() == 0; }

  void refreshResponseDrainedState() {
    if (responseDrained_ && allFramesDrained()) {
      state_ = LinkOperationState::responseDrained;
    }
  }

  void clearReleaseAcknowledgements() {
    completionAckPending_ = false;
    requestReleaseAckPending_ = false;
    coordinatorReleaseAckPending_ = false;
  }

  static bool ordinaryWorkState(LinkOperationState state) {
    return state == LinkOperationState::receiving ||
        state == LinkOperationState::processing ||
        state == LinkOperationState::durableCommit ||
        state == LinkOperationState::cleanup ||
        state == LinkOperationState::rollback;
  }

  static bool nonterminalFrameAllowed(LinkOperationState state) {
    return state == LinkOperationState::accepted ||
        state == LinkOperationState::receiving ||
        state == LinkOperationState::processing ||
        state == LinkOperationState::durableCommit ||
        state == LinkOperationState::cleanup ||
        state == LinkOperationState::rollback;
  }

  static bool terminalAllowed(LinkOperationState state) {
    return state == LinkOperationState::accepted ||
        state == LinkOperationState::processing ||
        state == LinkOperationState::durableCommit ||
        state == LinkOperationState::cleanup ||
        state == LinkOperationState::rollback;
  }

  static bool allowedWorkTransition(LinkOperationState from,
                                    LinkOperationState to) {
    if (from == to) return true;
    switch (from) {
      case LinkOperationState::accepted:
        return to == LinkOperationState::receiving ||
            to == LinkOperationState::processing ||
            to == LinkOperationState::durableCommit ||
            to == LinkOperationState::cleanup ||
            to == LinkOperationState::rollback;
      case LinkOperationState::receiving:
        return to == LinkOperationState::processing ||
            to == LinkOperationState::durableCommit ||
            to == LinkOperationState::cleanup ||
            to == LinkOperationState::rollback;
      case LinkOperationState::processing:
        return to == LinkOperationState::durableCommit ||
            to == LinkOperationState::cleanup ||
            to == LinkOperationState::rollback;
      case LinkOperationState::durableCommit:
        return to == LinkOperationState::cleanup ||
            to == LinkOperationState::rollback;
      case LinkOperationState::cancelled:
        return to == LinkOperationState::rollback ||
            to == LinkOperationState::cleanup;
      case LinkOperationState::rollback:
        return to == LinkOperationState::cleanup;
      case LinkOperationState::idle:
      case LinkOperationState::cleanup:
      case LinkOperationState::terminalResponse:
      case LinkOperationState::responseDrained:
      case LinkOperationState::blocked:
      case LinkOperationState::release:
        return false;
    }
    return false;
  }

  LinkOperationState state_ = LinkOperationState::idle;
  LinkOperationCancelReason cancelReason_ = LinkOperationCancelReason::none;
  uint32_t requestId_ = 0;
  LinkTransport transport_ = LinkTransport::none;
  uint32_t connectionGeneration_ = 0;
  uint32_t absoluteDeadlineMs_ = 0;
  uint16_t resources_ = 0;
  uint16_t progressFramesQueued_ = 0;
  uint16_t dataFramesQueued_ = 0;
  bool deadlineArmed_ = false;
  bool responseAllowed_ = true;
  bool responseDrained_ = false;
  bool completionEligible_ = false;
  bool cancelled_ = false;
  bool blocked_ = false;
  bool retainCoordinator_ = false;
  bool terminalFrameQueued_ = false;
  bool completionAckPending_ = false;
  bool requestReleaseAckPending_ = false;
  bool coordinatorReleaseAckPending_ = false;
};

}  // namespace pokepod
