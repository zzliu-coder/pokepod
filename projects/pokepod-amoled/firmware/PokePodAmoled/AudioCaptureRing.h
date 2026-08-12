#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifndef ARDUINO
#include <atomic>
#endif

namespace pokepod {

// The ESP32-S3 has naturally atomic aligned 32-bit loads/stores.  This ring is
// single-producer/single-consumer, so every counter has exactly one writer and
// only needs an explicit memory barrier when ownership crosses cores.  Using
// std::atomic here would fall back to a non-lock-free implementation in the
// Xtensa toolchain and would make the realtime capture path wait on a hidden
// runtime lock.
class AudioSpscCounter {
 public:
  uint32_t loadRelaxed() const {
#ifdef ARDUINO
    return value_;
#else
    return value_.load(std::memory_order_relaxed);
#endif
  }
  uint32_t loadAcquire() const {
#ifdef ARDUINO
    const uint32_t value = value_;
    __asm__ __volatile__("memw" ::: "memory");
    return value;
#else
    return value_.load(std::memory_order_acquire);
#endif
  }
  void storeRelaxed(uint32_t value) {
#ifdef ARDUINO
    value_ = value;
#else
    value_.store(value, std::memory_order_relaxed);
#endif
  }
  void storeRelease(uint32_t value) {
#ifdef ARDUINO
    __asm__ __volatile__("memw" ::: "memory");
    value_ = value;
#else
    value_.store(value, std::memory_order_release);
#endif
  }
  void beginPublication(uint32_t value) {
#ifdef ARDUINO
    value_ = value;
    __asm__ __volatile__("memw" ::: "memory");
#else
    value_.store(value, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_seq_cst);
#endif
  }
  void incrementWriter() { storeRelaxed(loadRelaxed() + 1); }

 private:
#ifdef ARDUINO
  alignas(4) volatile uint32_t value_ = 0;
#else
  std::atomic<uint32_t> value_{0};
#endif
};

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

  void resetSession(uint32_t sessionId) {
    readCount_.storeRelaxed(0);
    writeCount_.storeRelaxed(0);
    sessionId_ = sessionId;
    highWaterFrames_.storeRelaxed(0);
    pushedFrames_.storeRelaxed(0);
    poppedFrames_.storeRelaxed(0);
    droppedFrames_.storeRelaxed(0);
  }

  bool push(uint32_t sequence, uint32_t capturedAtMs,
            const int16_t *samples, size_t sampleCount) {
    if (samples == nullptr || sampleCount != kAudioCaptureSamplesPerFrame) {
      return false;
    }
    const uint32_t write = writeCount_.loadRelaxed();
    const uint32_t read = readCount_.loadAcquire();
    if (write - read >= Capacity) {
      droppedFrames_.incrementWriter();
      return false;
    }
    AudioCaptureFrame &frame = frames_[write % Capacity];
    frame.sessionId = sessionId_;
    frame.sequence = sequence;
    frame.capturedAtMs = capturedAtMs;
    memcpy(frame.samples, samples, sizeof(frame.samples));
    writeCount_.storeRelease(write + 1);
    pushedFrames_.incrementWriter();
    updateHighWater(write + 1 - read);
    return true;
  }

  bool pop(AudioCaptureFrame &frame) {
    const uint32_t read = readCount_.loadRelaxed();
    if (read == writeCount_.loadAcquire()) return false;
    frame = frames_[read % Capacity];
    readCount_.storeRelease(read + 1);
    poppedFrames_.incrementWriter();
    return true;
  }

  size_t size() const {
    return writeCount_.loadAcquire() - readCount_.loadAcquire();
  }

  constexpr size_t capacity() const { return Capacity; }

  AudioCaptureRingMetrics metrics() const {
    AudioCaptureRingMetrics value;
    value.sessionId = sessionId_;
    value.currentFrames = static_cast<uint32_t>(size());
    value.highWaterFrames = highWaterFrames_.loadAcquire();
    value.pushedFrames = pushedFrames_.loadAcquire();
    value.poppedFrames = poppedFrames_.loadAcquire();
    value.droppedFrames = droppedFrames_.loadAcquire();
    value.droppedSamples = value.droppedFrames * kAudioCaptureSamplesPerFrame;
    return value;
  }

 private:
  void updateHighWater(size_t depth) {
    const uint32_t current = highWaterFrames_.loadRelaxed();
    const uint32_t requested = static_cast<uint32_t>(depth);
    if (requested > current) highWaterFrames_.storeRelaxed(requested);
  }

  AudioCaptureFrame frames_[Capacity] = {};
  AudioSpscCounter readCount_;
  AudioSpscCounter writeCount_;
  uint32_t sessionId_ = 0;
  AudioSpscCounter highWaterFrames_;
  AudioSpscCounter pushedFrames_;
  AudioSpscCounter poppedFrames_;
  AudioSpscCounter droppedFrames_;
};

}  // namespace pokepod
