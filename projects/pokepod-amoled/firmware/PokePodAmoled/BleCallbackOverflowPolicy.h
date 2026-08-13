#pragma once

#include "BleVoiceCallbackMailbox.h"

namespace pokepod {

// An overflow begins a fail-closed physical disconnect. Logical service state
// must keep treating the exact epoch as connected until the callback-side
// latch proves that the controller has actually closed it.
class BleCallbackOverflowPolicy {
 public:
  void begin(const BleVoiceConnectionEpoch &epoch) {
    if (pending_) return;
    pending_ = true;
    epoch_ = epoch;
  }

  bool pending() const { return pending_; }
  bool physicalConnectionPending() const {
    return pending_ && epoch_.valid();
  }
  const BleVoiceConnectionEpoch &epoch() const { return epoch_; }

  bool confirm(const BleVoiceConnectionEpoch &physicalDisconnect) const {
    return pending_ &&
        (!epoch_.valid() || physicalDisconnect.matches(epoch_));
  }

  BleVoiceConnectionEpoch finish() {
    const BleVoiceConnectionEpoch closed = epoch_;
    pending_ = false;
    epoch_ = {};
    return closed;
  }

 private:
  bool pending_ = false;
  BleVoiceConnectionEpoch epoch_;
};

}  // namespace pokepod
