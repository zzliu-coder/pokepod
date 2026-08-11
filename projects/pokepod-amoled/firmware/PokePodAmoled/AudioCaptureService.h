#pragma once

#include <stddef.h>
#include <stdint.h>

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

  bool startSession(uint32_t sessionId, AudioCaptureSource &source) {
    if (running_ || sessionId == 0 || !source.start()) return false;
    source_ = &source;
    running_ = true;
    sessionId_ = sessionId;
    nextSequence_ = 0;
    rawUsed_ = 0;
    monoUsed_ = 0;
    readCalls_ = 0;
    shortReads_ = 0;
    timeouts_ = 0;
    sourceOverruns_ = 0;
    sourceFailures_ = 0;
    longestReadUs_ = 0;
    lastReadUs_ = 0;
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
  }

  AudioCaptureCycleResult captureOnce(uint32_t nowMs) {
    if (!running_ || source_ == nullptr) return AudioCaptureCycleResult::idle;
    AudioCaptureReadResult read = source_->readStereo48(
        raw_ + rawUsed_, sizeof(raw_) - rawUsed_, kReadTimeoutMs);
    ++readCalls_;
    lastReadUs_ = read.elapsedUs;
    if (read.elapsedUs > longestReadUs_) longestReadUs_ = read.elapsedUs;
    if (read.bytes > sizeof(raw_) - rawUsed_ || read.bytes % 4 != 0) {
      ++sourceFailures_;
      return AudioCaptureCycleResult::sourceFailure;
    }
    if (read.status == AudioCaptureReadStatus::timeout) {
      ++timeouts_;
      return AudioCaptureCycleResult::sourceTimeout;
    }
    if (read.status == AudioCaptureReadStatus::failure) {
      ++sourceFailures_;
      return AudioCaptureCycleResult::sourceFailure;
    }
    if (read.status == AudioCaptureReadStatus::overrun) ++sourceOverruns_;
    if (read.bytes == 0) {
      ++shortReads_;
      return read.status == AudioCaptureReadStatus::overrun
          ? AudioCaptureCycleResult::sourceOverrun
          : AudioCaptureCycleResult::partialInput;
    }
    rawUsed_ += read.bytes;
    if (rawUsed_ < sizeof(raw_)) {
      ++shortReads_;
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
    value.readCalls = readCalls_;
    value.shortReads = shortReads_;
    value.timeouts = timeouts_;
    value.sourceOverruns = sourceOverruns_;
    value.sourceFailures = sourceFailures_;
    value.longestReadUs = longestReadUs_;
    value.lastReadUs = lastReadUs_;
    value.partialMonoSamples = static_cast<uint32_t>(monoUsed_);
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
    }
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
  uint32_t readCalls_ = 0;
  uint32_t shortReads_ = 0;
  uint32_t timeouts_ = 0;
  uint32_t sourceOverruns_ = 0;
  uint32_t sourceFailures_ = 0;
  uint32_t longestReadUs_ = 0;
  uint32_t lastReadUs_ = 0;
};

}  // namespace pokepod
