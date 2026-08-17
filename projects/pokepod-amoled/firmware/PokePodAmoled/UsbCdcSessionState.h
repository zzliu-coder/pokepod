#pragma once

#include <atomic>

namespace pokepod {

struct UsbCdcSessionSnapshot {
  uint32_t generation = 0;
  bool active = false;
};

// TinyUSB reports physical mount and CDC line state independently. Keep the
// DTR-derived host session state in atomics because USB events run on the
// Arduino event task while the Link service is owned by the main loop.
class UsbCdcSessionState {
 public:
  void reset() {
    state_.store(0, std::memory_order_release);
    closedGeneration_.store(0, std::memory_order_release);
    closedPending_.store(false, std::memory_order_release);
  }

  void lineState(bool dtr) {
    uint32_t state = state_.load(std::memory_order_acquire);
    for (;;) {
      const bool wasActive = (state & 1U) != 0;
      const uint32_t generation = state >> 1;
      if (wasActive == dtr) return;

      uint32_t nextState = 0;
      if (dtr) {
        uint32_t nextGeneration = generation + 1U;
        if (nextGeneration == 0 || nextGeneration > kMaxGeneration) {
          nextGeneration = 1;
        }
        nextState = (nextGeneration << 1) | 1U;
      } else {
        nextState = generation << 1;
      }
      if (state_.compare_exchange_weak(state, nextState,
                                       std::memory_order_acq_rel,
                                       std::memory_order_acquire)) {
        if (!dtr) {
          closedGeneration_.store(generation, std::memory_order_release);
          closedPending_.store(true, std::memory_order_release);
        }
        return;
      }
    }
  }

  void disconnected() { lineState(false); }

  bool active() const {
    return snapshot().active;
  }

  UsbCdcSessionSnapshot snapshot() const {
    const uint32_t state = state_.load(std::memory_order_acquire);
    return UsbCdcSessionSnapshot{state >> 1, (state & 1U) != 0};
  }

  bool takeClosed(uint32_t &generation) {
    if (!closedPending_.exchange(false, std::memory_order_acq_rel)) {
      generation = 0;
      return false;
    }
    generation = closedGeneration_.load(std::memory_order_acquire);
    return generation != 0;
  }

 private:
  static constexpr uint32_t kMaxGeneration = 0x7fffffffU;
  // generation occupies bits 31..1 and the active DTR fact occupies bit 0.
  // A single atomic word makes a fast close/reopen observable as a new epoch
  // even when the main loop never observes the brief inactive state.
  std::atomic<uint32_t> state_{0};
  std::atomic<uint32_t> closedGeneration_{0};
  std::atomic<bool> closedPending_{false};
};

}  // namespace pokepod
