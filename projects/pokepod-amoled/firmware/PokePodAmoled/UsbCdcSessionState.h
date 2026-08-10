#pragma once

#include <atomic>

namespace pokepod {

// TinyUSB reports physical mount and CDC line state independently. Keep the
// DTR-derived host session state in atomics because USB events run on the
// Arduino event task while the Link service is owned by the main loop.
class UsbCdcSessionState {
 public:
  void reset() {
    active_.store(false, std::memory_order_release);
    closedPending_.store(false, std::memory_order_release);
  }

  void lineState(bool dtr) {
    const bool wasActive = active_.exchange(dtr, std::memory_order_acq_rel);
    if (wasActive && !dtr) {
      closedPending_.store(true, std::memory_order_release);
    }
  }

  void disconnected() { lineState(false); }

  bool active() const {
    return active_.load(std::memory_order_acquire);
  }

  bool takeClosed() {
    return closedPending_.exchange(false, std::memory_order_acq_rel);
  }

 private:
  std::atomic<bool> active_{false};
  std::atomic<bool> closedPending_{false};
};

}  // namespace pokepod
