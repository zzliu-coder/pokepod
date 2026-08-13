#pragma once

#include <stdint.h>

namespace pokepod {

enum class BleServiceEnablePhase : uint8_t {
  disabled,
  enabled,
  stoppingSession,
  disconnecting,
};

struct BleServiceEnableActions {
  bool stopAdvertising = false;
  bool requestSessionStop = false;
  bool disconnect = false;
  bool clearRuntime = false;
  bool startAdvertising = false;
};

// Bridges a level-triggered teardown policy to the App's asynchronous capture
// stop. Acknowledgement means the App accepted ownership of that stop; later
// policy polls must not enqueue it again while the same transition drains.
class BleSessionStopRequestLatch {
 public:
  void beginTransition(bool alreadyPending) {
    if (!alreadyPending) acknowledged_ = false;
  }

  void request() {
    if (!acknowledged_) requested_ = true;
  }

  bool requested() const { return requested_; }

  void acknowledge() {
    requested_ = false;
    acknowledged_ = true;
  }

 private:
  bool requested_ = false;
  bool acknowledged_ = false;
};

// Pure policy for the persistent BLE Voice user intent. The transport keeps
// its GATT objects alive while disabled so enabling is safe and inexpensive.
// A disable never skips an active session or fabricates a disconnect; it
// retries the physical disconnect until the BLE callback confirms it.
class BleServiceEnablePolicy {
 public:
  static constexpr uint32_t kDisconnectRetryMs = 250;

  void begin(bool enabled) {
    userEnabled_ = enabled;
    phase_ = enabled ? BleServiceEnablePhase::enabled
                     : BleServiceEnablePhase::disabled;
    disconnectRequested_ = false;
    lastDisconnectRequestMs_ = 0;
  }

  BleServiceEnableActions requestEnable(bool connected) {
    BleServiceEnableActions actions;
    if (userEnabled_) return actions;
    userEnabled_ = true;
    if (phase_ == BleServiceEnablePhase::disabled) {
      phase_ = BleServiceEnablePhase::enabled;
      disconnectRequested_ = false;
      lastDisconnectRequestMs_ = 0;
      actions.startAdvertising = !connected;
    }
    // A new enable intent cannot cancel teardown of the old connection or
    // session generation. poll() finishes it before accepting new work.
    return actions;
  }

  BleServiceEnableActions requestDisable(bool sessionActive, bool connected,
                                         uint32_t nowMs) {
    BleServiceEnableActions actions;
    actions.stopAdvertising = true;
    userEnabled_ = false;
    if (sessionActive) {
      phase_ = BleServiceEnablePhase::stoppingSession;
      actions.requestSessionStop = true;
      return actions;
    }
    return advanceTeardown(connected, nowMs, actions);
  }

  BleServiceEnableActions poll(bool sessionActive, bool connected,
                               uint32_t nowMs) {
    BleServiceEnableActions actions;
    if (phase_ == BleServiceEnablePhase::enabled ||
        phase_ == BleServiceEnablePhase::disabled) {
      return actions;
    }
    if (sessionActive) {
      phase_ = BleServiceEnablePhase::stoppingSession;
      actions.requestSessionStop = true;
      return actions;
    }
    return advanceTeardown(connected, nowMs, actions);
  }

  bool userEnabled() const { return userEnabled_; }
  bool acceptsNewWork() const {
    return userEnabled_ && phase_ == BleServiceEnablePhase::enabled;
  }
  bool disablePending() const {
    return !userEnabled_ && phase_ != BleServiceEnablePhase::disabled;
  }
  bool transitionPending() const {
    return phase_ == BleServiceEnablePhase::stoppingSession ||
        phase_ == BleServiceEnablePhase::disconnecting;
  }
  BleServiceEnablePhase phase() const { return phase_; }

 private:
  BleServiceEnableActions advanceTeardown(
      bool connected, uint32_t nowMs, BleServiceEnableActions actions) {
    if (!connected) {
      const bool firstTerminalArrival =
          phase_ != BleServiceEnablePhase::disabled &&
          phase_ != BleServiceEnablePhase::enabled;
      phase_ = userEnabled_ ? BleServiceEnablePhase::enabled
                            : BleServiceEnablePhase::disabled;
      disconnectRequested_ = false;
      lastDisconnectRequestMs_ = 0;
      actions.clearRuntime = firstTerminalArrival;
      actions.startAdvertising = firstTerminalArrival && userEnabled_;
      return actions;
    }
    phase_ = BleServiceEnablePhase::disconnecting;
    if (!disconnectRequested_ ||
        nowMs - lastDisconnectRequestMs_ >= kDisconnectRetryMs) {
      disconnectRequested_ = true;
      lastDisconnectRequestMs_ = nowMs;
      actions.disconnect = true;
    }
    return actions;
  }

  bool userEnabled_ = true;
  bool disconnectRequested_ = false;
  BleServiceEnablePhase phase_ = BleServiceEnablePhase::enabled;
  uint32_t lastDisconnectRequestMs_ = 0;
};

}  // namespace pokepod
