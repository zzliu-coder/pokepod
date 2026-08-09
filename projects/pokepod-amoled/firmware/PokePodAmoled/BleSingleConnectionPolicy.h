#pragma once

#include <stdint.h>

namespace pokepod {

constexpr uint16_t kInvalidBleConnectionId = 0xffff;

enum class BleConnectDecision : uint8_t {
  accepted,
  alreadyCurrent,
  rejectSecondary,
};

// The Arduino BLE stack can be built with more than one controller
// connection. PokePod Voice deliberately exposes one logical Mac connection,
// so every callback must pass this gate before it can mutate service state.
class BleSingleConnectionPolicy {
 public:
  BleConnectDecision connect(uint16_t connectionId) {
    if (connectionId == kInvalidBleConnectionId) {
      return BleConnectDecision::rejectSecondary;
    }
    if (!hasCurrent_) {
      hasCurrent_ = true;
      currentConnectionId_ = connectionId;
      return BleConnectDecision::accepted;
    }
    return currentConnectionId_ == connectionId
        ? BleConnectDecision::alreadyCurrent
        : BleConnectDecision::rejectSecondary;
  }

  bool disconnect(uint16_t connectionId) {
    if (!isCurrent(connectionId)) return false;
    hasCurrent_ = false;
    currentConnectionId_ = 0;
    return true;
  }

  bool isCurrent(uint16_t connectionId) const {
    return hasCurrent_ && currentConnectionId_ == connectionId;
  }

  bool commandAllowed(uint16_t connectionId) const {
    return isCurrent(connectionId);
  }

  bool authenticationAllowed(uint16_t connectionId) const {
    return isCurrent(connectionId);
  }

  void requestPairingAfterDisconnect() { pairingPending_ = true; }
  void cancelPairingRequest() { pairingPending_ = false; }

  bool consumePairingAfterDisconnect() {
    if (!pairingPending_ || hasCurrent_) return false;
    pairingPending_ = false;
    return true;
  }

  bool hasCurrent() const { return hasCurrent_; }
  uint16_t currentConnectionId() const { return currentConnectionId_; }
  bool pairingPending() const { return pairingPending_; }

 private:
  bool hasCurrent_ = false;
  bool pairingPending_ = false;
  uint16_t currentConnectionId_ = 0;
};

}  // namespace pokepod
