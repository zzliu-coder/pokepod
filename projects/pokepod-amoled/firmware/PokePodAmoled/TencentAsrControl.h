#pragma once

#include <atomic>
#include <stdint.h>

namespace pokepod {

enum class TencentAsrStage : uint8_t {
  idle = 0,
  validating,
  hashing,
  resolving,
  connecting,
  uploading,
  waitingResponse,
  parsing,
  completed,
  cancelled,
};

class TencentCancelToken {
 public:
  uint32_t begin() {
    uint32_t next = generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (next == 0) {
      generation_.store(1, std::memory_order_release);
      next = 1;
    }
    cancelledGeneration_.store(0, std::memory_order_release);
    return next;
  }

  bool cancel(uint32_t generation) {
    if (generation == 0 ||
        generation_.load(std::memory_order_acquire) != generation) {
      return false;
    }
    cancelledGeneration_.store(generation, std::memory_order_release);
    return true;
  }

  bool cancelled(uint32_t generation) const {
    return generation != 0 &&
        cancelledGeneration_.load(std::memory_order_acquire) == generation;
  }

 private:
  std::atomic<uint32_t> generation_{0};
  std::atomic<uint32_t> cancelledGeneration_{0};
};

struct TencentAsrControl {
  TencentCancelToken *cancelToken = nullptr;
  uint32_t generation = 0;
  std::atomic<uint8_t> *stage = nullptr;

  bool cancelled() const {
    return cancelToken != nullptr && cancelToken->cancelled(generation);
  }

  void setStage(TencentAsrStage value) const {
    if (stage != nullptr) {
      stage->store(static_cast<uint8_t>(value), std::memory_order_release);
    }
  }
};

}  // namespace pokepod
