#pragma once

#include <stddef.h>
#include <stdint.h>
#include <atomic>

#include "AudioCaptureRing.h"
#include "AudioFrontEnd.h"

namespace pokepod {

enum class AudioCaptureReadStatus : uint8_t {
  ok,
  timeout,
  overrun,
  failure,
};

struct AudioCaptureReadResult {
  AudioCaptureReadStatus status = AudioCaptureReadStatus::failure;
  size_t bytes = 0;
  uint32_t elapsedUs = 0;
};

// Platform adapter implemented by the later integration lane. start/stop own
// the I2S RX hardware; readStereo48() may return a partial block and must be
// bounded by timeoutMs. No logging or allocation is permitted from these calls.
class AudioCaptureSource {
 public:
  virtual ~AudioCaptureSource() = default;
  virtual bool start() = 0;
  virtual void stop() = 0;
  virtual AudioCaptureReadResult readStereo48(uint8_t *output,
                                              size_t capacity,
                                              uint32_t timeoutMs) = 0;
};

enum class AudioCaptureCycleResult : uint8_t {
  idle,
  partialInput,
  frameQueued,
  frameDropped,
  sourceTimeout,
  sourceOverrun,
  sourceFailure,
};

struct AudioCaptureServiceMetrics {
  AudioCaptureRingMetrics ring;
  uint32_t readCalls = 0;
  uint32_t shortReads = 0;
  uint32_t timeouts = 0;
  uint32_t sourceOverruns = 0;
  uint32_t sourceFailures = 0;
  uint32_t longestReadUs = 0;
  uint32_t lastReadUs = 0;
  uint32_t partialMonoSamples = 0;
};

// Task-ready capture core. The integration lane owns the single high-priority
// FreeRTOS task and calls captureOnce() from that task. Consumers call pop()
// from the main loop. This class contains only fixed storage and bounded calls.
template <size_t RingFrames = 6>
class AudioCaptureService {
 public:
  static constexpr size_t kRawStereoBytesPerFrame =
      48000 * kAudioCaptureFrameDurationMs / 1000 * 2 * sizeof(int16_t);
  static constexpr uint32_t kReadTimeoutMs = kAudioCaptureFrameDurationMs + 5;

  bool configureDspProfile(AudioDspProfile profile) {
    if (running_ || profile == AudioDspProfile::unavailable) return false;
    frontEnd_.configure(profile);
    return true;
  }

  bool startSession(uint32_t sessionId, AudioCaptureSource &source) {
    if (running_ || sessionId == 0 || !source.start()) return false;
    source_ = &source;
    running_ = true;
    sessionId_ = sessionId;
    nextSequence_ = 0;
    rawUsed_ = 0;
    monoUsed_ = 0;
    readCalls_.store(0, std::memory_order_relaxed);
    shortReads_.store(0, std::memory_order_relaxed);
    timeouts_.store(0, std::memory_order_relaxed);
    sourceOverruns_.store(0, std::memory_order_relaxed);
    sourceFailures_.store(0, std::memory_order_relaxed);
    longestReadUs_.store(0, std::memory_order_relaxed);
    lastReadUs_.store(0, std::memory_order_relaxed);
    partialMonoSamples_.store(0, std::memory_order_relaxed);
    frontEnd_.reset();
    ring_.resetSession(sessionId);
    return true;
  }

  void stopSession() {
    if (source_ != nullptr) source_->stop();
    source_ = nullptr;
    running_ = false;
    rawUsed_ = 0;
    monoUsed_ = 0;
    partialMonoSamples_.store(0, std::memory_order_relaxed);
  }

  AudioCaptureCycleResult captureOnce(uint32_t nowMs) {
    if (!running_ || source_ == nullptr) return AudioCaptureCycleResult::idle;
    AudioCaptureReadResult read = source_->readStereo48(
        raw_ + rawUsed_, sizeof(raw_) - rawUsed_, kReadTimeoutMs);
    readCalls_.fetch_add(1, std::memory_order_relaxed);
    lastReadUs_.store(read.elapsedUs, std::memory_order_relaxed);
    uint32_t longest = longestReadUs_.load(std::memory_order_relaxed);
    while (read.elapsedUs > longest &&
           !longestReadUs_.compare_exchange_weak(
               longest, read.elapsedUs, std::memory_order_relaxed,
               std::memory_order_relaxed)) {}
    if (read.bytes > sizeof(raw_) - rawUsed_ || read.bytes % 4 != 0) {
      sourceFailures_.fetch_add(1, std::memory_order_relaxed);
      return AudioCaptureCycleResult::sourceFailure;
    }
    if (read.status == AudioCaptureReadStatus::timeout) {
      timeouts_.fetch_add(1, std::memory_order_relaxed);
      return AudioCaptureCycleResult::sourceTimeout;
    }
    if (read.status == AudioCaptureReadStatus::failure) {
      sourceFailures_.fetch_add(1, std::memory_order_relaxed);
      return AudioCaptureCycleResult::sourceFailure;
    }
    if (read.status == AudioCaptureReadStatus::overrun) {
      sourceOverruns_.fetch_add(1, std::memory_order_relaxed);
    }
    if (read.bytes == 0) {
      shortReads_.fetch_add(1, std::memory_order_relaxed);
      return read.status == AudioCaptureReadStatus::overrun
          ? AudioCaptureCycleResult::sourceOverrun
          : AudioCaptureCycleResult::partialInput;
    }
    rawUsed_ += read.bytes;
    if (rawUsed_ < sizeof(raw_)) {
      shortReads_.fetch_add(1, std::memory_order_relaxed);
      return read.status == AudioCaptureReadStatus::overrun
          ? AudioCaptureCycleResult::sourceOverrun
          : AudioCaptureCycleResult::partialInput;
    }

    const size_t convertedBytes = frontEnd_.processStereo16(
        raw_, sizeof(raw_), converted_, sizeof(converted_));
    rawUsed_ = 0;
    appendConverted(converted_, convertedBytes, nowMs);
    if (lastFrameDropped_) return AudioCaptureCycleResult::frameDropped;
    if (lastFrameQueued_) return AudioCaptureCycleResult::frameQueued;
    return read.status == AudioCaptureReadStatus::overrun
        ? AudioCaptureCycleResult::sourceOverrun
        : AudioCaptureCycleResult::partialInput;
  }

  bool pop(AudioCaptureFrame &frame) { return ring_.pop(frame); }
  bool running() const { return running_; }
  uint32_t sessionId() const { return sessionId_; }
  const AudioFrontEndMetrics &frontEndMetrics() const {
    return frontEnd_.metrics();
  }

  AudioCaptureServiceMetrics metrics() const {
    AudioCaptureServiceMetrics value;
    value.ring = ring_.metrics();
    value.readCalls = readCalls_.load(std::memory_order_relaxed);
    value.shortReads = shortReads_.load(std::memory_order_relaxed);
    value.timeouts = timeouts_.load(std::memory_order_relaxed);
    value.sourceOverruns = sourceOverruns_.load(std::memory_order_relaxed);
    value.sourceFailures = sourceFailures_.load(std::memory_order_relaxed);
    value.longestReadUs = longestReadUs_.load(std::memory_order_relaxed);
    value.lastReadUs = lastReadUs_.load(std::memory_order_relaxed);
    value.partialMonoSamples =
        partialMonoSamples_.load(std::memory_order_relaxed);
    return value;
  }

 private:
  static int16_t decode(const uint8_t *bytes) {
    return static_cast<int16_t>(static_cast<uint16_t>(bytes[0]) |
                                static_cast<uint16_t>(bytes[1]) << 8);
  }

  void appendConverted(const uint8_t *bytes, size_t count, uint32_t nowMs) {
    lastFrameQueued_ = false;
    lastFrameDropped_ = false;
    for (size_t offset = 0; offset + 1 < count; offset += 2) {
      mono_[monoUsed_++] = decode(bytes + offset);
      if (monoUsed_ != kAudioCaptureSamplesPerFrame) continue;
      if (ring_.push(nextSequence_, nowMs, mono_, monoUsed_)) {
        lastFrameQueued_ = true;
      } else {
        lastFrameDropped_ = true;
      }
      ++nextSequence_;
      monoUsed_ = 0;
      partialMonoSamples_.store(0, std::memory_order_relaxed);
    }
    partialMonoSamples_.store(static_cast<uint32_t>(monoUsed_),
                              std::memory_order_relaxed);
  }

  AudioCaptureSource *source_ = nullptr;
  AudioFrontEnd frontEnd_;
  AudioCaptureRing<RingFrames> ring_;
  uint8_t raw_[kRawStereoBytesPerFrame] = {};
  uint8_t converted_[kAudioCaptureSamplesPerFrame * sizeof(int16_t)] = {};
  int16_t mono_[kAudioCaptureSamplesPerFrame] = {};
  size_t rawUsed_ = 0;
  size_t monoUsed_ = 0;
  bool running_ = false;
  bool lastFrameQueued_ = false;
  bool lastFrameDropped_ = false;
  uint32_t sessionId_ = 0;
  uint32_t nextSequence_ = 0;
  std::atomic<uint32_t> readCalls_{0};
  std::atomic<uint32_t> shortReads_{0};
  std::atomic<uint32_t> timeouts_{0};
  std::atomic<uint32_t> sourceOverruns_{0};
  std::atomic<uint32_t> sourceFailures_{0};
  std::atomic<uint32_t> longestReadUs_{0};
  std::atomic<uint32_t> lastReadUs_{0};
  std::atomic<uint32_t> partialMonoSamples_{0};
};

}  // namespace pokepod
