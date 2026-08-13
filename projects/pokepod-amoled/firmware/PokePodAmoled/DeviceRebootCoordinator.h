#pragma once

#include <stdint.h>

#include "LinkServiceCoordinator.h"

namespace pokepod {

enum class DeviceRebootPhase : uint8_t {
  idle,
  accepted,
  waitingForServices,
  ready,
};

// Device-level authority for an accepted reboot. USB and Wi-Fi Link services
// submit here, while the application owns quiescence and ESP.restart(). A
// transport disconnect therefore cannot cancel an accepted device intent.
class DeviceRebootCoordinator {
 public:
  bool request(uint32_t nowMs, LinkTransport transport) {
    if (transport == LinkTransport::none || pending()) return false;
    origin_ = transport;
    dueAtMs_ = nowMs + kResponseDrainMs;
    phase_ = DeviceRebootPhase::accepted;
    return true;
  }

  bool pending() const { return phase_ != DeviceRebootPhase::idle; }
  bool due(uint32_t nowMs) const {
    return pending() && static_cast<int32_t>(nowMs - dueAtMs_) >= 0;
  }
  bool ready() const { return phase_ == DeviceRebootPhase::ready; }
  DeviceRebootPhase phase() const { return phase_; }
  LinkTransport origin() const { return origin_; }

  bool beginServiceQuiesce() {
    if (phase_ != DeviceRebootPhase::accepted) return false;
    phase_ = DeviceRebootPhase::waitingForServices;
    return true;
  }

  void defer(uint32_t nowMs, uint32_t delayMs = kServicePollMs) {
    if (!pending()) return;
    dueAtMs_ = nowMs + delayMs;
  }

  void retryServiceQuiesce(uint32_t nowMs) {
    if (phase_ != DeviceRebootPhase::waitingForServices) return;
    phase_ = DeviceRebootPhase::accepted;
    dueAtMs_ = nowMs + kQuiesceRetryMs;
  }

  bool markReady() {
    if (phase_ != DeviceRebootPhase::waitingForServices) return false;
    phase_ = DeviceRebootPhase::ready;
    return true;
  }

  void acknowledgeRestart() {
    phase_ = DeviceRebootPhase::idle;
    origin_ = LinkTransport::none;
    dueAtMs_ = 0;
  }

  static constexpr uint32_t kResponseDrainMs = 100;
  static constexpr uint32_t kServicePollMs = 20;
  static constexpr uint32_t kQuiesceRetryMs = 100;

 private:
  DeviceRebootPhase phase_ = DeviceRebootPhase::idle;
  LinkTransport origin_ = LinkTransport::none;
  uint32_t dueAtMs_ = 0;
};

}  // namespace pokepod
