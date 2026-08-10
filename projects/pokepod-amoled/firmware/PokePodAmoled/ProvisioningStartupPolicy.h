#pragma once

#include <stdint.h>

#include "MonotonicTime.h"

namespace pokepod {

enum class ProvisioningStartupPhase : uint8_t {
  idle,
  requested,
  quiescing,
  switchingMode,
  startingAccessPoint,
  active,
  failed,
};

enum class ProvisioningStartupAction : uint8_t {
  none,
  quiesceRadio,
  switchRadioMode,
  startAccessPoint,
  startPortalServices,
  failTimeout,
};

inline const char *provisioningStartupPhaseName(
    ProvisioningStartupPhase phase) {
  switch (phase) {
    case ProvisioningStartupPhase::idle: return "idle";
    case ProvisioningStartupPhase::requested: return "requested";
    case ProvisioningStartupPhase::quiescing: return "quiescing";
    case ProvisioningStartupPhase::switchingMode: return "switching-mode";
    case ProvisioningStartupPhase::startingAccessPoint:
      return "starting-access-point";
    case ProvisioningStartupPhase::active: return "active";
    case ProvisioningStartupPhase::failed: return "failed";
  }
  return "idle";
}

class ProvisioningStartupPolicy {
 public:
  static constexpr uint32_t kRequestSettleMs = 32;
  static constexpr uint32_t kMinimumSettleMs = 120;
  static constexpr uint32_t kModeSettleMs = 120;
  static constexpr uint32_t kAccessPointSettleMs = 120;
  static constexpr uint32_t kStartupTimeoutMs = 4000;

  bool request(uint32_t nowMs) {
    if (phase_ != ProvisioningStartupPhase::idle) return false;
    requestedAtMs_ = nowMs;
    quiescedAtMs_ = 0;
    stepStartedAtMs_ = 0;
    phase_ = ProvisioningStartupPhase::requested;
    pendingAction_ = ProvisioningStartupAction::none;
    return true;
  }

  ProvisioningStartupAction update(uint32_t nowMs) {
    if (phase_ == ProvisioningStartupPhase::idle ||
        phase_ == ProvisioningStartupPhase::active ||
        phase_ == ProvisioningStartupPhase::failed) {
      return ProvisioningStartupAction::none;
    }
    if (monotonicElapsedAtLeast(nowMs, requestedAtMs_, kStartupTimeoutMs)) {
      phase_ = ProvisioningStartupPhase::failed;
      pendingAction_ = ProvisioningStartupAction::none;
      return ProvisioningStartupAction::failTimeout;
    }
    if (pendingAction_ != ProvisioningStartupAction::none) {
      return ProvisioningStartupAction::none;
    }
    if (phase_ == ProvisioningStartupPhase::requested) {
      if (!monotonicElapsedAtLeast(nowMs, requestedAtMs_, kRequestSettleMs)) {
        return ProvisioningStartupAction::none;
      }
      phase_ = ProvisioningStartupPhase::quiescing;
      quiescedAtMs_ = nowMs;
      return ProvisioningStartupAction::quiesceRadio;
    }
    if (phase_ == ProvisioningStartupPhase::quiescing &&
        monotonicElapsedAtLeast(nowMs, quiescedAtMs_, kMinimumSettleMs)) {
      pendingAction_ = ProvisioningStartupAction::switchRadioMode;
      return pendingAction_;
    }
    if (phase_ == ProvisioningStartupPhase::switchingMode &&
        monotonicElapsedAtLeast(nowMs, stepStartedAtMs_, kModeSettleMs)) {
      pendingAction_ = ProvisioningStartupAction::startAccessPoint;
      return pendingAction_;
    }
    if (phase_ == ProvisioningStartupPhase::startingAccessPoint &&
        monotonicElapsedAtLeast(nowMs, stepStartedAtMs_,
                                kAccessPointSettleMs)) {
      pendingAction_ = ProvisioningStartupAction::startPortalServices;
      return pendingAction_;
    }
    return ProvisioningStartupAction::none;
  }

  void finishStep(ProvisioningStartupAction action, bool succeeded,
                  uint32_t nowMs) {
    if (action == ProvisioningStartupAction::none ||
        action != pendingAction_) {
      return;
    }
    pendingAction_ = ProvisioningStartupAction::none;
    if (!succeeded) {
      phase_ = ProvisioningStartupPhase::failed;
      return;
    }
    stepStartedAtMs_ = nowMs;
    if (action == ProvisioningStartupAction::switchRadioMode) {
      phase_ = ProvisioningStartupPhase::switchingMode;
    } else if (action == ProvisioningStartupAction::startAccessPoint) {
      phase_ = ProvisioningStartupPhase::startingAccessPoint;
    } else if (action == ProvisioningStartupAction::startPortalServices) {
      phase_ = ProvisioningStartupPhase::active;
    }
  }

  void fail() {
    if (phase_ == ProvisioningStartupPhase::idle ||
        phase_ == ProvisioningStartupPhase::active) return;
    phase_ = ProvisioningStartupPhase::failed;
  }

  void reset() {
    phase_ = ProvisioningStartupPhase::idle;
    requestedAtMs_ = 0;
    quiescedAtMs_ = 0;
    stepStartedAtMs_ = 0;
    pendingAction_ = ProvisioningStartupAction::none;
  }

  ProvisioningStartupPhase phase() const { return phase_; }
  bool pending() const {
    return phase_ == ProvisioningStartupPhase::requested ||
        phase_ == ProvisioningStartupPhase::quiescing ||
        phase_ == ProvisioningStartupPhase::switchingMode ||
        phase_ == ProvisioningStartupPhase::startingAccessPoint;
  }
  bool active() const {
    return phase_ == ProvisioningStartupPhase::active;
  }
  bool failed() const {
    return phase_ == ProvisioningStartupPhase::failed;
  }
  bool visible() const { return phase_ != ProvisioningStartupPhase::idle; }
  bool ownsWifi() const { return visible(); }

 private:
  ProvisioningStartupPhase phase_ = ProvisioningStartupPhase::idle;
  uint32_t requestedAtMs_ = 0;
  uint32_t quiescedAtMs_ = 0;
  uint32_t stepStartedAtMs_ = 0;
  ProvisioningStartupAction pendingAction_ = ProvisioningStartupAction::none;
};

}  // namespace pokepod
