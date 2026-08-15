#pragma once

#include <stddef.h>
#include <stdint.h>
#include <atomic>

#include "AudioCaptureRing.h"
#include "AudioCaptureTiming.h"
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

// Platform adapter implemented by the integration lane. start/stop own the
// I2S RX hardware; readStereo48() may return a partial block and is bounded by
// the session-level kAudioCaptureReadTimeoutMs configured on the driver. No
// logging or allocation is permitted from these calls.
class AudioCaptureSource {
 public:
  virtual ~AudioCaptureSource() = default;
  virtual bool start() = 0;
  virtual void stop() = 0;
  // Some platform adapters can distinguish a DMA overrun from a timeout or
  // short read. Adapters that only expose a byte count must leave this false;
  // a zero overrun counter then means "not observable", not "no loss".
  virtual bool overrunObservable() const { return false; }
  virtual AudioCaptureReadResult readStereo48(uint8_t *output,
                                              size_t capacity) = 0;
};

enum class AudioCaptureCycleResult : uint8_t {
  idle,
  partialInput,
  frameQueued,
  frameDropped,
  sourceTimeout,
  sourceEarlyZero,
  sourceOverrun,
  sourceFailure,
};

struct AudioCaptureServiceMetrics {
  AudioCaptureRingMetrics ring;
  bool sourceOverrunObservable = false;
  uint32_t readCalls = 0;
  uint32_t shortReads = 0;
  uint32_t timeouts = 0;
  uint32_t zeroByteReads = 0;
  uint32_t earlyZeroReads = 0;
  uint32_t sourceOverruns = 0;
  uint32_t sourceFailures = 0;
  uint32_t longestReadUs = 0;
  uint32_t lastReadUs = 0;
  uint32_t partialMonoSamples = 0;
};

// Cross-core diagnostics are published separately from AudioFrontEnd's live
// state.  Every payload field is a 32-bit atomic and sequence is odd while the
// realtime owner is publishing.  Readers therefore obtain one coherent
// session snapshot without racing the DSP object or holding a lock in the
// capture task.
struct AudioCaptureFrontEndSnapshot {
  uint32_t generation = 0;
  uint32_t sessionId = 0;
  bool active = false;
  AudioDspProfile profile = AudioDspProfile::unavailable;
  AudioInputChannel selectedChannel = AudioInputChannel::undecided;
  uint32_t clippedInputSamples = 0;
  uint16_t leftPeak = 0;
  uint16_t rightPeak = 0;
  uint32_t gatedSamples = 0;
  uint32_t suppressedSamples = 0;
  uint32_t limitedSamples = 0;
  uint16_t outputPeak = 0;
  uint16_t estimatedNoiseFloor = 0;
  uint32_t maximumGainQ12 = 4096;

  AudioFrontEndMetrics asMetrics() const {
    AudioFrontEndMetrics value;
    value.profile = profile;
    value.selectedChannel = selectedChannel;
    value.clippedInputSamples = clippedInputSamples;
    value.leftPeak = leftPeak;
    value.rightPeak = rightPeak;
    value.gatedSamples = gatedSamples;
    value.suppressedSamples = suppressedSamples;
    value.limitedSamples = limitedSamples;
    value.outputPeak = outputPeak;
    value.estimatedNoiseFloor = estimatedNoiseFloor;
    value.maximumGainQ12 = maximumGainQ12;
    return value;
  }
};

class AudioCaptureFrontEndPublisher {
 public:
  AudioCaptureFrontEndPublisher() {
    profile_.storeRelaxed(
        static_cast<uint32_t>(AudioDspProfile::unavailable));
    maximumGainQ12_.storeRelaxed(4096);
  }

  void publish(uint32_t sessionId, bool active,
               const AudioFrontEndMetrics &metrics) {
    const uint32_t before = sequence_.loadRelaxed();
    sequence_.beginPublication(before + 1U);
    sessionId_.storeRelaxed(sessionId);
    active_.storeRelaxed(active ? 1U : 0U);
    profile_.storeRelaxed(static_cast<uint32_t>(metrics.profile));
    selectedChannel_.storeRelaxed(
        static_cast<uint32_t>(metrics.selectedChannel));
    clippedInputSamples_.storeRelaxed(metrics.clippedInputSamples);
    leftPeak_.storeRelaxed(metrics.leftPeak);
    rightPeak_.storeRelaxed(metrics.rightPeak);
    gatedSamples_.storeRelaxed(metrics.gatedSamples);
    suppressedSamples_.storeRelaxed(metrics.suppressedSamples);
    limitedSamples_.storeRelaxed(metrics.limitedSamples);
    outputPeak_.storeRelaxed(metrics.outputPeak);
    estimatedNoiseFloor_.storeRelaxed(metrics.estimatedNoiseFloor);
    maximumGainQ12_.storeRelaxed(metrics.maximumGainQ12);
    sequence_.storeRelease(before + 2U);
  }

  AudioCaptureFrontEndSnapshot snapshot() const {
    AudioCaptureFrontEndSnapshot value;
    for (;;) {
      const uint32_t before = sequence_.loadAcquire();
      if ((before & 1U) != 0) continue;
      value.sessionId = sessionId_.loadRelaxed();
      value.active = active_.loadRelaxed() != 0;
      value.profile = static_cast<AudioDspProfile>(
          profile_.loadRelaxed());
      value.selectedChannel = static_cast<AudioInputChannel>(
          selectedChannel_.loadRelaxed());
      value.clippedInputSamples =
          clippedInputSamples_.loadRelaxed();
      value.leftPeak = static_cast<uint16_t>(
          leftPeak_.loadRelaxed());
      value.rightPeak = static_cast<uint16_t>(
          rightPeak_.loadRelaxed());
      value.gatedSamples = gatedSamples_.loadRelaxed();
      value.suppressedSamples =
          suppressedSamples_.loadRelaxed();
      value.limitedSamples = limitedSamples_.loadRelaxed();
      value.outputPeak = static_cast<uint16_t>(
          outputPeak_.loadRelaxed());
      value.estimatedNoiseFloor = static_cast<uint16_t>(
          estimatedNoiseFloor_.loadRelaxed());
      value.maximumGainQ12 =
          maximumGainQ12_.loadRelaxed();
      // The validation load below must stay after every payload read.  Put a
      // read barrier here so the reader cannot accept an unchanged even
      // sequence while one payload load already comes from the next publish.
      finishPayloadRead();
      const uint32_t after = sequence_.loadAcquire();
      if (before == after && (after & 1U) == 0) {
        value.generation = after / 2U;
        return value;
      }
    }
  }

 private:
  static void finishPayloadRead() {
#ifdef ARDUINO
    __asm__ __volatile__("memw" ::: "memory");
#else
    std::atomic_thread_fence(std::memory_order_acquire);
#endif
  }

  AudioSpscCounter sequence_;
  AudioSpscCounter sessionId_;
  AudioSpscCounter active_;
  AudioSpscCounter profile_;
  AudioSpscCounter selectedChannel_;
  AudioSpscCounter clippedInputSamples_;
  AudioSpscCounter leftPeak_;
  AudioSpscCounter rightPeak_;
  AudioSpscCounter gatedSamples_;
  AudioSpscCounter suppressedSamples_;
  AudioSpscCounter limitedSamples_;
  AudioSpscCounter outputPeak_;
  AudioSpscCounter estimatedNoiseFloor_;
  AudioSpscCounter maximumGainQ12_;
};

// Task-ready capture core. The integration lane owns the single high-priority
// FreeRTOS task and calls captureOnce() from that task. Consumers call pop()
// from the main loop. This class contains only fixed storage and bounded calls.
template <size_t RingFrames = 6>
class AudioCaptureService {
 public:
  static constexpr size_t kRawStereoBytesPerFrame =
      48000 * kAudioCaptureFrameDurationMs / 1000 * 2 * sizeof(int16_t);
  bool configureDspProfile(AudioDspProfile profile) {
    if (running_ || profile == AudioDspProfile::unavailable) return false;
    frontEnd_.configure(profile);
    return true;
  }

  bool startSession(uint32_t sessionId, AudioCaptureSource &source) {
    if (running_ || sessionId == 0 || !source.start()) return false;
    source_ = &source;
    sourceOverrunObservable_ = source.overrunObservable();
    running_ = true;
    sessionId_ = sessionId;
    nextSequence_ = 0;
    rawUsed_ = 0;
    monoUsed_ = 0;
    readCalls_.store(0, std::memory_order_relaxed);
    shortReads_.store(0, std::memory_order_relaxed);
    timeouts_.store(0, std::memory_order_relaxed);
    zeroByteReads_.store(0, std::memory_order_relaxed);
    earlyZeroReads_.store(0, std::memory_order_relaxed);
    sourceOverruns_.store(0, std::memory_order_relaxed);
    sourceFailures_.store(0, std::memory_order_relaxed);
    longestReadUs_.store(0, std::memory_order_relaxed);
    lastReadUs_.store(0, std::memory_order_relaxed);
    partialMonoSamples_.store(0, std::memory_order_relaxed);
    frontEnd_.reset();
    ring_.resetSession(sessionId);
    frontEndPublisher_.publish(sessionId_, true, frontEnd_.metrics());
    return true;
  }

  void stopSession() {
    if (source_ != nullptr) source_->stop();
    source_ = nullptr;
    running_ = false;
    rawUsed_ = 0;
    monoUsed_ = 0;
    partialMonoSamples_.store(0, std::memory_order_relaxed);
    frontEndPublisher_.publish(sessionId_, false, frontEnd_.metrics());
  }

  AudioCaptureCycleResult captureOnce(uint32_t nowMs) {
    if (!running_ || source_ == nullptr) return AudioCaptureCycleResult::idle;
    AudioCaptureReadResult read = source_->readStereo48(
        raw_ + rawUsed_, sizeof(raw_) - rawUsed_);
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
    const bool zeroByteRead = read.bytes == 0;
    const bool earlyZero = zeroByteRead &&
        read.elapsedUs < kAudioCaptureReadTimeoutMs * 1000U;
    if (zeroByteRead) {
      zeroByteReads_.fetch_add(1, std::memory_order_relaxed);
      if (earlyZero) {
        earlyZeroReads_.fetch_add(1, std::memory_order_relaxed);
      }
    }
    if (read.status == AudioCaptureReadStatus::timeout) {
      timeouts_.fetch_add(1, std::memory_order_relaxed);
      return earlyZero ? AudioCaptureCycleResult::sourceEarlyZero
                       : AudioCaptureCycleResult::sourceTimeout;
    }
    if (read.status == AudioCaptureReadStatus::failure) {
      sourceFailures_.fetch_add(1, std::memory_order_relaxed);
      return AudioCaptureCycleResult::sourceFailure;
    }
    if (read.status == AudioCaptureReadStatus::overrun) {
      sourceOverruns_.fetch_add(1, std::memory_order_relaxed);
    }
    if (zeroByteRead) {
      shortReads_.fetch_add(1, std::memory_order_relaxed);
      return earlyZero ? AudioCaptureCycleResult::sourceEarlyZero
          : read.status == AudioCaptureReadStatus::overrun
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
    frontEndPublisher_.publish(sessionId_, true, frontEnd_.metrics());
    rawUsed_ = 0;
    appendConverted(converted_, convertedBytes, nowMs);
    // A driver overrun makes the session incomplete even when the bytes that
    // remain happen to form a publishable 20 ms frame. Keep that frame for
    // diagnostics/tail continuity, while giving data-loss evidence priority
    // over the queue outcome returned to the realtime owner.
    if (read.status == AudioCaptureReadStatus::overrun) {
      return AudioCaptureCycleResult::sourceOverrun;
    }
    if (lastFrameDropped_) return AudioCaptureCycleResult::frameDropped;
    if (lastFrameQueued_) return AudioCaptureCycleResult::frameQueued;
    return AudioCaptureCycleResult::partialInput;
  }

  bool pop(AudioCaptureFrame &frame) { return ring_.pop(frame); }
  bool running() const { return running_; }
  uint32_t sessionId() const { return sessionId_; }
  AudioCaptureFrontEndSnapshot frontEndSnapshot() const {
    return frontEndPublisher_.snapshot();
  }

  AudioCaptureServiceMetrics metrics() const {
    AudioCaptureServiceMetrics value;
    value.ring = ring_.metrics();
    value.sourceOverrunObservable = sourceOverrunObservable_;
    value.readCalls = readCalls_.load(std::memory_order_relaxed);
    value.shortReads = shortReads_.load(std::memory_order_relaxed);
    value.timeouts = timeouts_.load(std::memory_order_relaxed);
    value.zeroByteReads = zeroByteReads_.load(std::memory_order_relaxed);
    value.earlyZeroReads = earlyZeroReads_.load(std::memory_order_relaxed);
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
  AudioCaptureFrontEndPublisher frontEndPublisher_;
  AudioCaptureRing<RingFrames> ring_;
  uint8_t raw_[kRawStereoBytesPerFrame] = {};
  uint8_t converted_[kAudioCaptureSamplesPerFrame * sizeof(int16_t)] = {};
  int16_t mono_[kAudioCaptureSamplesPerFrame] = {};
  size_t rawUsed_ = 0;
  size_t monoUsed_ = 0;
  bool running_ = false;
  bool lastFrameQueued_ = false;
  bool lastFrameDropped_ = false;
  bool sourceOverrunObservable_ = false;
  uint32_t sessionId_ = 0;
  uint32_t nextSequence_ = 0;
  std::atomic<uint32_t> readCalls_{0};
  std::atomic<uint32_t> shortReads_{0};
  std::atomic<uint32_t> timeouts_{0};
  std::atomic<uint32_t> zeroByteReads_{0};
  std::atomic<uint32_t> earlyZeroReads_{0};
  std::atomic<uint32_t> sourceOverruns_{0};
  std::atomic<uint32_t> sourceFailures_{0};
  std::atomic<uint32_t> longestReadUs_{0};
  std::atomic<uint32_t> lastReadUs_{0};
  std::atomic<uint32_t> partialMonoSamples_{0};
};

}  // namespace pokepod
