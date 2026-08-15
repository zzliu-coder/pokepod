#pragma once

#include <atomic>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace pokepod {

// The capture task already has a short internal handoff ring.  This second,
// PSRAM-backed SPSC queue decouples the Arduino/UI loop from all synchronous
// SD primitives.  128 full 20 ms mono frames retain 2.56 seconds.
constexpr size_t kRecorderStorageFrameBytes = 640;
constexpr size_t kRecorderStorageQueueFrames = 128;
constexpr size_t kRecorderQualificationHighWaterFrames =
    kRecorderStorageQueueFrames * 3U / 4U;
constexpr size_t kRecorderStorageQueueSlots =
    kRecorderStorageQueueFrames + 1;

struct RecorderStorageFrame {
  uint16_t length = 0;
  uint8_t bytes[kRecorderStorageFrameBytes]{};
};

class RecorderStorageQueue {
 public:
  bool bind(RecorderStorageFrame *slots, size_t slotCount) {
    if (slots == nullptr || slotCount < kRecorderStorageQueueSlots) {
      return false;
    }
    slots_ = slots;
    slotCount_ = slotCount;
    reset();
    return true;
  }

  void reset() {
    head_.store(0, std::memory_order_relaxed);
    tail_.store(0, std::memory_order_relaxed);
    highWater_.store(0, std::memory_order_relaxed);
    dropped_.store(0, std::memory_order_relaxed);
  }

  bool push(const uint8_t *bytes, size_t length) {
    if (slots_ == nullptr || bytes == nullptr || length == 0 ||
        length > kRecorderStorageFrameBytes) {
      return false;
    }
    const size_t head = head_.load(std::memory_order_relaxed);
    const size_t next = (head + 1U) % slotCount_;
    if (next == tail_.load(std::memory_order_acquire)) {
      dropped_.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    slots_[head].length = static_cast<uint16_t>(length);
    memcpy(slots_[head].bytes, bytes, length);
    head_.store(next, std::memory_order_release);
    const size_t depth = size();
    size_t prior = highWater_.load(std::memory_order_relaxed);
    while (depth > prior && !highWater_.compare_exchange_weak(
        prior, depth, std::memory_order_relaxed)) {}
    return true;
  }

  bool pop(RecorderStorageFrame &frame) {
    if (slots_ == nullptr) return false;
    const size_t tail = tail_.load(std::memory_order_relaxed);
    if (tail == head_.load(std::memory_order_acquire)) return false;
    frame = slots_[tail];
    tail_.store((tail + 1U) % slotCount_, std::memory_order_release);
    return true;
  }

  size_t size() const {
    const size_t head = head_.load(std::memory_order_acquire);
    const size_t tail = tail_.load(std::memory_order_acquire);
    return head >= tail ? head - tail : slotCount_ - tail + head;
  }
  bool empty() const { return size() == 0; }
  size_t highWater() const {
    return highWater_.load(std::memory_order_relaxed);
  }
  uint32_t dropped() const {
    return dropped_.load(std::memory_order_relaxed);
  }

 private:
  RecorderStorageFrame *slots_ = nullptr;
  size_t slotCount_ = 0;
  std::atomic<size_t> head_{0};
  std::atomic<size_t> tail_{0};
  std::atomic<size_t> highWater_{0};
  std::atomic<uint32_t> dropped_{0};
};

static_assert(kRecorderStorageQueueFrames * 20U >= 2500U,
              "storage queue must cover at least 2.5 seconds");
static_assert(kRecorderQualificationHighWaterFrames == 96U,
              "qualification invalidation keeps 640 ms queue headroom");

}  // namespace pokepod
