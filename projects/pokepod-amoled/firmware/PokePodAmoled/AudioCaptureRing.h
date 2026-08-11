#pragma once

#include <atomic>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace pokepod {

constexpr uint32_t kAudioCaptureSampleRate = 16000;
constexpr uint16_t kAudioCaptureFrameDurationMs = 20;
constexpr size_t kAudioCaptureSamplesPerFrame =
    kAudioCaptureSampleRate * kAudioCaptureFrameDurationMs / 1000;

static_assert(kAudioCaptureSamplesPerFrame == 320,
              "capture frames must remain 20 ms at 16 kHz");

struct AudioCaptureFrame {
  uint32_t sessionId = 0;
  uint32_t sequence = 0;
  uint32_t capturedAtMs = 0;
  int16_t samples[kAudioCaptureSamplesPerFrame] = {};
};

struct AudioCaptureRingMetrics {
  uint32_t sessionId = 0;
  uint32_t currentFrames = 0;
  uint32_t highWaterFrames = 0;
  uint32_t pushedFrames = 0;
  uint32_t poppedFrames = 0;
  uint32_t droppedFrames = 0;
  uint32_t droppedSamples = 0;

  bool incomplete() const { return droppedFrames != 0; }
};

// Fixed-capacity single-producer/single-consumer ring. The producer owns tail
// and publishes it with release ordering; the consumer owns head and observes
// it with acquire ordering. resetSession() may only be called while both sides
// are stopped. The realtime path performs no allocation and never waits.
template <size_t Capacity>
class AudioCaptureRing {
 public:
  static_assert(Capacity > 0, "audio capture ring must be bounded");
  static_assert(std::atomic<uint32_t>::is_always_lock_free,
                "realtime ring counters must be lock-free");

  void resetSession(uint32_t sessionId) {
    readCount_.store(0, std::memory_order_relaxed);
    writeCount_.store(0, std::memory_order_relaxed);
    sessionId_ = sessionId;
    highWaterFrames_.store(0, std::memory_order_relaxed);
    pushedFrames_.store(0, std::memory_order_relaxed);
    poppedFrames_.store(0, std::memory_order_relaxed);
    droppedFrames_.store(0, std::memory_order_relaxed);
  }

  bool push(uint32_t sequence, uint32_t capturedAtMs,
            const int16_t *samples, size_t sampleCount) {
    if (samples == nullptr || sampleCount != kAudioCaptureSamplesPerFrame) {
      return false;
    }
    const uint32_t write = writeCount_.load(std::memory_order_relaxed);
    const uint32_t read = readCount_.load(std::memory_order_acquire);
    if (write - read >= Capacity) {
      droppedFrames_.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    AudioCaptureFrame &frame = frames_[write % Capacity];
    frame.sessionId = sessionId_;
    frame.sequence = sequence;
    frame.capturedAtMs = capturedAtMs;
    memcpy(frame.samples, samples, sizeof(frame.samples));
    writeCount_.store(write + 1, std::memory_order_release);
    pushedFrames_.fetch_add(1, std::memory_order_relaxed);
    updateHighWater(write + 1 - read);
    return true;
  }

  bool pop(AudioCaptureFrame &frame) {
    const uint32_t read = readCount_.load(std::memory_order_relaxed);
    if (read == writeCount_.load(std::memory_order_acquire)) return false;
    frame = frames_[read % Capacity];
    readCount_.store(read + 1, std::memory_order_release);
    poppedFrames_.fetch_add(1, std::memory_order_relaxed);
    return true;
  }

  size_t size() const {
    return writeCount_.load(std::memory_order_acquire) -
        readCount_.load(std::memory_order_acquire);
  }

  constexpr size_t capacity() const { return Capacity; }

  AudioCaptureRingMetrics metrics() const {
    AudioCaptureRingMetrics value;
    value.sessionId = sessionId_;
    value.currentFrames = static_cast<uint32_t>(size());
    value.highWaterFrames = highWaterFrames_.load(std::memory_order_relaxed);
    value.pushedFrames = pushedFrames_.load(std::memory_order_relaxed);
    value.poppedFrames = poppedFrames_.load(std::memory_order_relaxed);
    value.droppedFrames = droppedFrames_.load(std::memory_order_relaxed);
    value.droppedSamples = value.droppedFrames * kAudioCaptureSamplesPerFrame;
    return value;
  }

 private:
  void updateHighWater(size_t depth) {
    uint32_t current = highWaterFrames_.load(std::memory_order_relaxed);
    const uint32_t requested = static_cast<uint32_t>(depth);
    while (requested > current &&
           !highWaterFrames_.compare_exchange_weak(
               current, requested, std::memory_order_relaxed,
               std::memory_order_relaxed)) {}
  }

  AudioCaptureFrame frames_[Capacity] = {};
  std::atomic<uint32_t> readCount_{0};
  std::atomic<uint32_t> writeCount_{0};
  uint32_t sessionId_ = 0;
  std::atomic<uint32_t> highWaterFrames_{0};
  std::atomic<uint32_t> pushedFrames_{0};
  std::atomic<uint32_t> poppedFrames_{0};
  std::atomic<uint32_t> droppedFrames_{0};
};

}  // namespace pokepod
