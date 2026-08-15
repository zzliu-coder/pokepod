#pragma once

#include <atomic>
#include <stddef.h>
#include <stdint.h>

#include "AudioCaptureService.h"
#include "AudioCaptureDispatcher.h"

namespace pokepod {

constexpr size_t kAudioLatencyHistogramBuckets = 12;
static_assert(AudioCaptureDispatcherMetrics::kIntervalHistogramBuckets ==
                  kAudioLatencyHistogramBuckets,
              "dispatcher and session histogram shapes must match");
constexpr uint32_t kAudioLatencyBucketUpperUs[kAudioLatencyHistogramBuckets] = {
    250U, 500U, 1000U, 2000U, 4000U, 8000U,
    16000U, 32000U, 64000U, 128000U, 512000U, UINT32_MAX};

struct AudioSessionTelemetrySnapshot {
  uint32_t generation = 0;
  uint32_t sessionId = 0;
  uint32_t captureRingHighWaterFrames = 0;
  uint32_t captureRingDroppedFrames = 0;
  uint32_t recorderQueueHighWaterFrames = 0;
  uint32_t recorderQueueDroppedFrames = 0;
  uint32_t dispatcherMaximumIntervalUs = 0;
  uint32_t dispatcherP99IntervalUs = 0;
  uint32_t i2sTimeouts = 0;
  uint32_t i2sLongestReadUs = 0;
  uint32_t zeroByteReads = 0;
  uint32_t earlyZeroReads = 0;
  uint32_t sourceOverruns = 0;
  uint32_t sourceFailures = 0;
  uint32_t sequenceGaps = 0;
  uint32_t storageWriteP99Us = 0;
  uint32_t storageWriteP999Us = 0;
  uint32_t captureTaskStackHighWaterWords = 0;
  uint32_t recorderTaskStackHighWaterWords = 0;
  bool sourceOverrunObservable = false;
  bool frozen = false;

  bool incomplete() const {
    return captureRingDroppedFrames != 0 ||
        recorderQueueDroppedFrames != 0 || i2sTimeouts != 0 ||
        earlyZeroReads != 0 || sourceOverruns != 0 || sourceFailures != 0 ||
        sequenceGaps != 0;
  }
};

struct AudioSessionTelemetryProducer {
  uint32_t generation = 0;
  uint32_t sessionId = 0;

  bool valid() const { return generation != 0 && sessionId != 0; }
};

class AudioSessionTelemetry {
 public:
  void reset() {
    beginMutation(true);
    if (!frozen_.load(std::memory_order_relaxed) &&
        generation_.load(std::memory_order_relaxed) != 0) {
      endMutation();
      return;
    }
    uint32_t next = generation_.load(std::memory_order_relaxed) + 1U;
    if (next == 0) next = 1;
    frozen_.store(false, std::memory_order_relaxed);
    sessionId_.store(0, std::memory_order_relaxed);
    boundCaptureSessionId_.store(0, std::memory_order_relaxed);
    captureRingHighWater_.store(0, std::memory_order_relaxed);
    captureRingDrops_.store(0, std::memory_order_relaxed);
    recorderQueueHighWater_.store(0, std::memory_order_relaxed);
    recorderQueueDrops_.store(0, std::memory_order_relaxed);
    dispatcherMaximumIntervalUs_.store(0, std::memory_order_relaxed);
    i2sTimeouts_.store(0, std::memory_order_relaxed);
    i2sLongestReadUs_.store(0, std::memory_order_relaxed);
    zeroByteReads_.store(0, std::memory_order_relaxed);
    earlyZeroReads_.store(0, std::memory_order_relaxed);
    sourceOverruns_.store(0, std::memory_order_relaxed);
    sourceFailures_.store(0, std::memory_order_relaxed);
    sourceOverrunObservable_.store(false, std::memory_order_relaxed);
    sequenceGaps_.store(0, std::memory_order_relaxed);
    captureTaskStackHighWaterWords_.store(0, std::memory_order_relaxed);
    recorderTaskStackHighWaterWords_.store(0, std::memory_order_relaxed);
    resetHistogram(dispatcherHistogram_);
    resetHistogram(storageHistogram_);
    generation_.store(next, std::memory_order_relaxed);
    endMutation();
  }

  AudioSessionTelemetryProducer bindCaptureSession(uint32_t sessionId) {
    AudioSessionTelemetryProducer producer;
    if (sessionId == 0 || !beginMutation(false)) return producer;
    resetCaptureFacts();
    boundCaptureSessionId_.store(sessionId, std::memory_order_relaxed);
    sessionId_.store(sessionId, std::memory_order_relaxed);
    producer.generation = generation_.load(std::memory_order_relaxed);
    producer.sessionId = sessionId;
    endMutation();
    return producer;
  }

  void observeCapture(const AudioSessionTelemetryProducer &producer,
                      const AudioCaptureServiceMetrics &capture,
                      const AudioCaptureDispatcherMetrics &dispatcher,
                      uint32_t captureTaskStackHighWaterWords) {
    if (!producer.valid() || capture.ring.sessionId != producer.sessionId ||
        !beginMutation(false)) return;
    if (producer.generation != generation_.load(std::memory_order_relaxed) ||
        producer.sessionId !=
            boundCaptureSessionId_.load(std::memory_order_relaxed)) {
      endMutation();
      return;
    }
    updateMaximum(captureRingHighWater_, capture.ring.highWaterFrames);
    captureRingDrops_.store(capture.ring.droppedFrames,
                            std::memory_order_relaxed);
    i2sTimeouts_.store(capture.timeouts, std::memory_order_relaxed);
    i2sLongestReadUs_.store(capture.longestReadUs,
                            std::memory_order_relaxed);
    zeroByteReads_.store(capture.zeroByteReads, std::memory_order_relaxed);
    earlyZeroReads_.store(capture.earlyZeroReads, std::memory_order_relaxed);
    sourceOverruns_.store(capture.sourceOverruns, std::memory_order_relaxed);
    sourceFailures_.store(capture.sourceFailures, std::memory_order_relaxed);
    sourceOverrunObservable_.store(capture.sourceOverrunObservable,
                                   std::memory_order_relaxed);
    sequenceGaps_.store(dispatcher.sequenceFailures,
                        std::memory_order_relaxed);
    updateMaximum(dispatcherMaximumIntervalUs_,
                  dispatcher.maximumIntervalUs);
    for (size_t i = 0; i < kAudioLatencyHistogramBuckets; ++i) {
      dispatcherHistogram_[i].store(dispatcher.intervalHistogram[i],
                                    std::memory_order_relaxed);
    }
    updateMinimumNonZero(captureTaskStackHighWaterWords_,
                         captureTaskStackHighWaterWords);
    endMutation();
  }

  void observeRecorderQueue(uint32_t highWaterFrames, uint32_t droppedFrames,
                            uint32_t recorderTaskStackHighWaterWords) {
    if (!beginMutation(false)) return;
    updateMaximum(recorderQueueHighWater_, highWaterFrames);
    recorderQueueDrops_.store(droppedFrames, std::memory_order_relaxed);
    updateMinimumNonZero(recorderTaskStackHighWaterWords_,
                         recorderTaskStackHighWaterWords);
    endMutation();
  }

  void recordStorageWrite(uint32_t elapsedUs) {
    if (!beginMutation(false)) return;
    histogramIncrement(storageHistogram_, elapsedUs);
    endMutation();
  }

  void freeze() {
    // Taking the writer sequence is the producer acknowledgement: freeze waits
    // for any capture/storage merge already in flight, then closes the session
    // before later producers can mutate it.
    beginMutation(true);
    frozen_.store(true, std::memory_order_relaxed);
    endMutation();
  }

  AudioSessionTelemetrySnapshot snapshot() const {
    AudioSessionTelemetrySnapshot value;
    uint32_t before = 0;
    uint32_t after = 0;
    do {
      before = sequence_.load(std::memory_order_acquire);
      if ((before & 1U) != 0) continue;
      value.generation = generation_.load(std::memory_order_relaxed);
      value.sessionId = sessionId_.load(std::memory_order_relaxed);
      value.captureRingHighWaterFrames =
          captureRingHighWater_.load(std::memory_order_relaxed);
      value.captureRingDroppedFrames =
          captureRingDrops_.load(std::memory_order_relaxed);
      value.recorderQueueHighWaterFrames =
          recorderQueueHighWater_.load(std::memory_order_relaxed);
      value.recorderQueueDroppedFrames =
          recorderQueueDrops_.load(std::memory_order_relaxed);
      value.dispatcherMaximumIntervalUs =
          dispatcherMaximumIntervalUs_.load(std::memory_order_relaxed);
      value.dispatcherP99IntervalUs = percentile(dispatcherHistogram_, 990U);
      value.i2sTimeouts = i2sTimeouts_.load(std::memory_order_relaxed);
      value.i2sLongestReadUs =
          i2sLongestReadUs_.load(std::memory_order_relaxed);
      value.zeroByteReads = zeroByteReads_.load(std::memory_order_relaxed);
      value.earlyZeroReads = earlyZeroReads_.load(std::memory_order_relaxed);
      value.sourceOverruns = sourceOverruns_.load(std::memory_order_relaxed);
      value.sourceFailures = sourceFailures_.load(std::memory_order_relaxed);
      value.sequenceGaps = sequenceGaps_.load(std::memory_order_relaxed);
      value.storageWriteP99Us = percentile(storageHistogram_, 990U);
      value.storageWriteP999Us = percentile(storageHistogram_, 999U);
      value.captureTaskStackHighWaterWords =
          captureTaskStackHighWaterWords_.load(std::memory_order_relaxed);
      value.recorderTaskStackHighWaterWords =
          recorderTaskStackHighWaterWords_.load(std::memory_order_relaxed);
      value.sourceOverrunObservable =
          sourceOverrunObservable_.load(std::memory_order_relaxed);
      value.frozen = frozen_.load(std::memory_order_relaxed);
      after = sequence_.load(std::memory_order_acquire);
    } while (before != after || (after & 1U) != 0);
    return value;
  }

 private:
  using Histogram = std::atomic<uint32_t>[kAudioLatencyHistogramBuckets];

  static void resetHistogram(Histogram &histogram) {
    for (auto &bucket : histogram) bucket.store(0, std::memory_order_relaxed);
  }

  static size_t bucketFor(uint32_t value) {
    for (size_t i = 0; i < kAudioLatencyHistogramBuckets; ++i) {
      if (value <= kAudioLatencyBucketUpperUs[i]) return i;
    }
    return kAudioLatencyHistogramBuckets - 1U;
  }

  static void histogramIncrement(Histogram &histogram, uint32_t value) {
    histogram[bucketFor(value)].fetch_add(1, std::memory_order_relaxed);
  }

  static uint32_t percentile(const Histogram &histogram,
                             uint32_t permille) {
    uint64_t total = 0;
    for (const auto &bucket : histogram) {
      total += bucket.load(std::memory_order_relaxed);
    }
    if (total == 0) return 0;
    const uint64_t target = (total * permille + 999U) / 1000U;
    uint64_t cumulative = 0;
    for (size_t i = 0; i < kAudioLatencyHistogramBuckets; ++i) {
      cumulative += histogram[i].load(std::memory_order_relaxed);
      if (cumulative >= target) return kAudioLatencyBucketUpperUs[i];
    }
    return UINT32_MAX;
  }

  static void updateMaximum(std::atomic<uint32_t> &destination,
                            uint32_t value) {
    uint32_t prior = destination.load(std::memory_order_relaxed);
    while (value > prior && !destination.compare_exchange_weak(
        prior, value, std::memory_order_relaxed)) {}
  }

  static void updateMinimumNonZero(std::atomic<uint32_t> &destination,
                                   uint32_t value) {
    if (value == 0) return;
    uint32_t prior = destination.load(std::memory_order_relaxed);
    while ((prior == 0 || value < prior) && !destination.compare_exchange_weak(
        prior, value, std::memory_order_relaxed)) {}
  }

  bool beginMutation(bool allowFrozen) {
    uint32_t observed = sequence_.load(std::memory_order_acquire);
    for (;;) {
      if ((observed & 1U) == 0 && sequence_.compare_exchange_weak(
              observed, observed + 1U, std::memory_order_acquire,
              std::memory_order_relaxed)) {
        if (!allowFrozen && frozen_.load(std::memory_order_relaxed)) {
          endMutation();
          return false;
        }
        return true;
      }
      observed = sequence_.load(std::memory_order_acquire);
    }
  }

  void endMutation() {
    sequence_.fetch_add(1U, std::memory_order_release);
  }

  void resetCaptureFacts() {
    captureRingHighWater_.store(0, std::memory_order_relaxed);
    captureRingDrops_.store(0, std::memory_order_relaxed);
    dispatcherMaximumIntervalUs_.store(0, std::memory_order_relaxed);
    i2sTimeouts_.store(0, std::memory_order_relaxed);
    i2sLongestReadUs_.store(0, std::memory_order_relaxed);
    zeroByteReads_.store(0, std::memory_order_relaxed);
    earlyZeroReads_.store(0, std::memory_order_relaxed);
    sourceOverruns_.store(0, std::memory_order_relaxed);
    sourceFailures_.store(0, std::memory_order_relaxed);
    sourceOverrunObservable_.store(false, std::memory_order_relaxed);
    sequenceGaps_.store(0, std::memory_order_relaxed);
    captureTaskStackHighWaterWords_.store(0, std::memory_order_relaxed);
    resetHistogram(dispatcherHistogram_);
  }

  std::atomic<uint32_t> sequence_{0};
  std::atomic<uint32_t> generation_{0};
  std::atomic<uint32_t> sessionId_{0};
  std::atomic<uint32_t> boundCaptureSessionId_{0};
  std::atomic<uint32_t> captureRingHighWater_{0};
  std::atomic<uint32_t> captureRingDrops_{0};
  std::atomic<uint32_t> recorderQueueHighWater_{0};
  std::atomic<uint32_t> recorderQueueDrops_{0};
  std::atomic<uint32_t> dispatcherMaximumIntervalUs_{0};
  std::atomic<uint32_t> i2sTimeouts_{0};
  std::atomic<uint32_t> i2sLongestReadUs_{0};
  std::atomic<uint32_t> zeroByteReads_{0};
  std::atomic<uint32_t> earlyZeroReads_{0};
  std::atomic<uint32_t> sourceOverruns_{0};
  std::atomic<uint32_t> sourceFailures_{0};
  std::atomic<bool> sourceOverrunObservable_{false};
  std::atomic<uint32_t> sequenceGaps_{0};
  std::atomic<uint32_t> captureTaskStackHighWaterWords_{0};
  std::atomic<uint32_t> recorderTaskStackHighWaterWords_{0};
  Histogram dispatcherHistogram_{};
  Histogram storageHistogram_{};
  std::atomic<bool> frozen_{false};
};

}  // namespace pokepod
