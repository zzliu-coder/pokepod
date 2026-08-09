#pragma once

namespace pokepod {

// Connection-scoped authorization. A bond belonging to some other peer must
// never authorize the peer that is currently connected.
class BlePeerPolicy {
 public:
  void connected(bool currentPeerBonded) {
    currentPeerBonded_ = currentPeerBonded;
  }

  void disconnected() { currentPeerBonded_ = false; }
  void forgotBonds() { currentPeerBonded_ = false; }
  void authenticatedAndBonded() { currentPeerBonded_ = true; }

  bool securityAllowed(bool pairingMode) const {
    return currentPeerBonded_ || pairingMode;
  }

  bool commandAllowed(bool connected, bool authenticated,
                      bool pairingMode) const {
    return connected && authenticated && securityAllowed(pairingMode);
  }

  bool currentPeerBonded() const { return currentPeerBonded_; }

 private:
  bool currentPeerBonded_ = false;
};

}  // namespace pokepod
