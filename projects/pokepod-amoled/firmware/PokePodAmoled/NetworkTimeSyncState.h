#pragma once

#include <atomic>
#include <stdint.h>

namespace pokepod {

// SNTP notifications arrive on the lwIP event task. This small state object is
// the only cross-task surface: callbacks record a completed adjustment, while
// the Arduino loop owns the RTC write and all logging.
class NetworkTimeSyncState {
 public:
  void noteConnected(bool connected) {
    const bool wasConnected = connected_.exchange(connected);
    if (connected && !wasConnected) {
      connectionGeneration_.fetch_add(1);
    }
  }

  void noteSynchronized() {
    if (!connected_.load()) return;
    const uint32_t generation = connectionGeneration_.load();
    uint32_t previous = synchronizedGeneration_.load();
    while (previous != generation) {
      if (synchronizedGeneration_.compare_exchange_weak(previous,
                                                         generation)) {
        revision_.fetch_add(1);
        return;
      }
    }
  }

  uint32_t connectionGeneration() const {
    return connectionGeneration_.load();
  }
  uint32_t revision() const { return revision_.load(); }

 private:
  std::atomic<bool> connected_{false};
  std::atomic<uint32_t> connectionGeneration_{0};
  std::atomic<uint32_t> synchronizedGeneration_{0};
  std::atomic<uint32_t> revision_{0};
};

}  // namespace pokepod
