#pragma once

#include <stdint.h>

namespace pokepod {

enum class ProvisioningStartupPhase : uint8_t {
  idle,
  requested,
  quiescing,
  active,
  failed,
};

enum class ProvisioningStartupAction : uint8_t {
  none,
  quiesceRadio,
  startPortal,
  failTimeout,
};

inline const char *provisioningStartupPhaseName(
    ProvisioningStartupPhase phase) {
  switch (phase) {
    case ProvisioningStartupPhase::idle: return "idle";
    case ProvisioningStartupPhase::requested: return "requested";
    case ProvisioningStartupPhase::quiescing: return "quiescing";
    case ProvisioningStartupPhase::active: return "active";
    case ProvisioningStartupPhase::failed: return "failed";
  }
  return "idle";
}

class ProvisioningStartupPolicy {
 public:
  static constexpr uint32_t kRequestSettleMs = 32;
  static constexpr uint32_t kMinimumSettleMs = 120;
  static constexpr uint32_t kStartupTimeoutMs = 2500;

  bool request(uint32_t nowMs) {
    if (phase_ != ProvisioningStartupPhase::idle) return false;
    requestedAtMs_ = nowMs;
    quiescedAtMs_ = 0;
    phase_ = ProvisioningStartupPhase::requested;
    startIssued_ = false;
    return true;
  }

  ProvisioningStartupAction update(uint32_t nowMs, bool wifiReady) {
    if ((phase_ != ProvisioningStartupPhase::requested &&
         phase_ != ProvisioningStartupPhase::quiescing) || startIssued_) {
      return ProvisioningStartupAction::none;
    }
    const uint32_t elapsed = static_cast<uint32_t>(nowMs - requestedAtMs_);
    if (elapsed >= kStartupTimeoutMs) {
      phase_ = ProvisioningStartupPhase::failed;
      return ProvisioningStartupAction::failTimeout;
    }
    if (phase_ == ProvisioningStartupPhase::requested) {
      if (elapsed < kRequestSettleMs) return ProvisioningStartupAction::none;
      phase_ = ProvisioningStartupPhase::quiescing;
      quiescedAtMs_ = nowMs;
      return ProvisioningStartupAction::quiesceRadio;
    }
    const uint32_t quiescedFor = static_cast<uint32_t>(nowMs - quiescedAtMs_);
    if (wifiReady && quiescedFor >= kMinimumSettleMs) {
      startIssued_ = true;
      return ProvisioningStartupAction::startPortal;
    }
    return ProvisioningStartupAction::none;
  }

  void finishStart(bool succeeded) {
    if (phase_ != ProvisioningStartupPhase::quiescing || !startIssued_) return;
    phase_ = succeeded ? ProvisioningStartupPhase::active
                       : ProvisioningStartupPhase::failed;
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
    startIssued_ = false;
  }

  ProvisioningStartupPhase phase() const { return phase_; }
  bool pending() const {
    return phase_ == ProvisioningStartupPhase::requested ||
        phase_ == ProvisioningStartupPhase::quiescing;
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
  bool startIssued_ = false;
};

}  // namespace pokepod
