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

// Transport-independent request lifecycle.  It deliberately owns no socket,
// File or Arduino object: the production adapter publishes resource facts and
// performs the returned release actions.  Intermediate frames never mutate
// completion state.
class LinkOperation {
 public:
  LinkOperationAdmission admit(uint32_t requestId, LinkTransport transport,
                               uint32_t connectionGeneration,
                               bool coordinatorOwned,
                               bool deadlineArmed = false,
                               uint32_t absoluteDeadlineMs = 0) {
    if (requestId == 0 || transport == LinkTransport::none) {
      return LinkOperationAdmission::invalid;
    }
    if (active()) {
      return owns(requestId, transport, connectionGeneration)
          ? LinkOperationAdmission::sameRequest
          : LinkOperationAdmission::busy;
    }
    if (state_ != LinkOperationState::idle) {
      return LinkOperationAdmission::busy;
    }
    if (ownsResource(LinkOperationResource::coordinator) &&
        transport_ != transport) {
      return LinkOperationAdmission::busy;
    }
    requestId_ = requestId;
    transport_ = transport;
    connectionGeneration_ = connectionGeneration;
    deadlineArmed_ = deadlineArmed;
    absoluteDeadlineMs_ = absoluteDeadlineMs;
    cancelReason_ = LinkOperationCancelReason::none;
    responseAllowed_ = true;
    responseDrained_ = false;
    completionEligible_ = false;
    cancelled_ = false;
    resources_ |= linkOperationResourceBit(LinkOperationResource::request);
    if (coordinatorOwned) {
      resources_ |= linkOperationResourceBit(
          LinkOperationResource::coordinator);
    }
    state_ = LinkOperationState::accepted;
    return LinkOperationAdmission::accepted;
  }

  bool active() const {
    return requestId_ != 0 && state_ != LinkOperationState::idle &&
        state_ != LinkOperationState::release;
  }

  bool owns(uint32_t requestId, LinkTransport transport,
            uint32_t connectionGeneration) const {
    return active() && requestId_ == requestId && transport_ == transport &&
        connectionGeneration_ == connectionGeneration;
  }

  bool enter(LinkOperationState next) {
    if (!allowed(state_, next)) return false;
    state_ = next;
    return true;
  }

  bool observeDeadline(uint32_t nowMs) {
    if (!active() || !deadlineArmed_) return active();
    if (static_cast<int32_t>(nowMs - absoluteDeadlineMs_) < 0) return true;
    cancel(LinkOperationCancelReason::deadline);
    return false;
  }

  void cancel(LinkOperationCancelReason reason) {
    if (!active()) return;
    cancelReason_ = reason;
    cancelled_ = true;
    responseAllowed_ = false;
    if (!responseDrained_) completionEligible_ = false;
    state_ = LinkOperationState::cancelled;
  }

  bool queueFrame(LinkOperationFrameRole role,
                  bool completionEligible = false) {
    if (!active() || !responseAllowed_) return false;
    if (role != LinkOperationFrameRole::terminal) return true;
    if (!enter(LinkOperationState::terminalResponse)) return false;
    completionEligible_ = completionEligible;
    return true;
  }

  bool frameDrained(LinkOperationFrameRole role,
                    uint32_t connectionGeneration) {
    if (role != LinkOperationFrameRole::terminal) return true;
    if (connectionGeneration != connectionGeneration_ ||
        state_ != LinkOperationState::terminalResponse) return false;
    responseDrained_ = true;
    state_ = LinkOperationState::responseDrained;
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
    const bool terminal = responseDrained_;
    const bool cancelled = cancelled_ || state_ == LinkOperationState::blocked;
    if ((!terminal && !cancelled) || !operationalResourcesDrained()) {
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

  bool beginRelease() {
    const LinkOperationSettlement value = settlement();
    if (!value.ready) return false;
    if (value.releaseRequest) {
      releaseResource(LinkOperationResource::request);
    }
    if (value.releaseCoordinator) {
      releaseResource(LinkOperationResource::coordinator);
    }
    state_ = LinkOperationState::release;
    return true;
  }

  bool finishRelease() {
    if (state_ != LinkOperationState::release ||
        ownsResource(LinkOperationResource::request) ||
        !operationalResourcesDrained()) return false;
    requestId_ = 0;
    connectionGeneration_ = 0;
    deadlineArmed_ = false;
    absoluteDeadlineMs_ = 0;
    cancelReason_ = LinkOperationCancelReason::none;
    responseAllowed_ = true;
    responseDrained_ = false;
    completionEligible_ = false;
    cancelled_ = false;
    retainCoordinator_ = false;
    if (!ownsResource(LinkOperationResource::coordinator)) {
      transport_ = LinkTransport::none;
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

 private:
  static bool allowed(LinkOperationState from, LinkOperationState to) {
    if (from == to) return true;
    switch (from) {
      case LinkOperationState::accepted:
        return to == LinkOperationState::receiving ||
            to == LinkOperationState::processing ||
            to == LinkOperationState::durableCommit ||
            to == LinkOperationState::cleanup ||
            to == LinkOperationState::terminalResponse ||
            to == LinkOperationState::cancelled ||
            to == LinkOperationState::blocked;
      case LinkOperationState::receiving:
        return to == LinkOperationState::processing ||
            to == LinkOperationState::durableCommit ||
            to == LinkOperationState::cleanup ||
            to == LinkOperationState::cancelled ||
            to == LinkOperationState::blocked;
      case LinkOperationState::processing:
        return to == LinkOperationState::durableCommit ||
            to == LinkOperationState::cleanup ||
            to == LinkOperationState::rollback ||
            to == LinkOperationState::cancelled ||
            to == LinkOperationState::blocked;
      case LinkOperationState::durableCommit:
        return to == LinkOperationState::cleanup ||
            to == LinkOperationState::terminalResponse ||
            to == LinkOperationState::rollback ||
            to == LinkOperationState::cancelled ||
            to == LinkOperationState::blocked;
      case LinkOperationState::cleanup:
        return to == LinkOperationState::terminalResponse ||
            to == LinkOperationState::rollback ||
            to == LinkOperationState::cancelled ||
            to == LinkOperationState::blocked;
      case LinkOperationState::terminalResponse:
        return to == LinkOperationState::responseDrained ||
            to == LinkOperationState::cancelled;
      case LinkOperationState::responseDrained:
        return to == LinkOperationState::cancelled ||
            to == LinkOperationState::release;
      case LinkOperationState::cancelled:
        return to == LinkOperationState::rollback ||
            to == LinkOperationState::cleanup ||
            to == LinkOperationState::blocked ||
            to == LinkOperationState::release;
      case LinkOperationState::rollback:
        return to == LinkOperationState::cleanup ||
            to == LinkOperationState::blocked ||
            to == LinkOperationState::release;
      case LinkOperationState::blocked:
        return to == LinkOperationState::release;
      case LinkOperationState::idle:
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
  bool deadlineArmed_ = false;
  bool responseAllowed_ = true;
  bool responseDrained_ = false;
  bool completionEligible_ = false;
  bool cancelled_ = false;
  bool retainCoordinator_ = false;
};

}  // namespace pokepod
