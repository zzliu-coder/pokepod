#pragma once

#include <stddef.h>
#include <stdint.h>

#include "AudioCaptureRing.h"
#include "AudioCaptureRouter.h"
#include "RecorderOutcome.h"

namespace pokepod {

enum class AudioCaptureDispatchTarget : uint8_t {
  none = 0,
  localRecorder,
  linkRecorder,
  bleVoice,
};

inline AudioCaptureDispatchTarget audioCaptureDispatchTarget(
    AudioCaptureOwner captureOwner, RecorderOperationOwner recorderOwner,
    bool recorderRecording, bool bleAccepting) {
  if (captureOwner == AudioCaptureOwner::wirelessVoice) {
    return bleAccepting ? AudioCaptureDispatchTarget::bleVoice
                        : AudioCaptureDispatchTarget::none;
  }
  if (captureOwner != AudioCaptureOwner::localCapsule ||
      !recorderRecording) {
    return AudioCaptureDispatchTarget::none;
  }
  if (recorderOwner == RecorderOperationOwner::localApp) {
    return AudioCaptureDispatchTarget::localRecorder;
  }
  if (recorderOwner == RecorderOperationOwner::linkUsb ||
      recorderOwner == RecorderOperationOwner::linkWifi) {
    return AudioCaptureDispatchTarget::linkRecorder;
  }
  return AudioCaptureDispatchTarget::none;
}

struct AudioCaptureDispatchResult {
  bool ok = true;
  bool hadFrames = false;
  bool sequenceIncomplete = false;
  bool routingFailure = false;
  bool recorderDeliveryFailure = false;
  bool voiceDeliveryFailure = false;
  RecorderOperationOwner failedRecorderOwner = RecorderOperationOwner::none;
  uint32_t sessionId = 0;
  uint32_t consumedFrames = 0;
  uint32_t firstSequence = 0;
  uint32_t lastSequence = 0;
};

struct AudioCaptureDispatcherMetrics {
  static constexpr size_t kIntervalHistogramBuckets = 12;
  uint32_t sessionId = 0;
  uint32_t consumedFrames = 0;
  uint32_t sequenceFailures = 0;
  uint32_t routingFailures = 0;
  uint32_t recorderDeliveryFailures = 0;
  uint32_t voiceDeliveryFailures = 0;
  uint32_t maximumIntervalUs = 0;
  uint32_t intervalSamples = 0;
  uint32_t intervalHistogram[kIntervalHistogramBuckets]{};
};

// The sole consumer of AudioCaptureRuntime's SPSC ring. App and Link may both
// ask this object to drain, but neither can pop a frame directly. Routing is
// decided for every frame from the durable recorder owner plus the logical
// capture owner, so a Link recording can never be mistaken for a local UI
// recording merely because both use AudioCaptureOwner::localCapsule.
class AudioCaptureDispatcher {
 public:
  template <typename Source, typename Router, typename Audio,
            typename Recorder, typename Voice, typename Log>
  AudioCaptureDispatchResult drain(Source &source, const Router &router,
                                   Audio &audio, Recorder &recorder,
                                   Voice &voice, Log &log, uint32_t nowMs) {
    AudioCaptureDispatchResult result;
    AudioCaptureFrame frame;
    while (source.pop(frame)) {
      result.hadFrames = true;
      ++result.consumedFrames;
      if (result.consumedFrames == 1U) result.firstSequence = frame.sequence;
      result.lastSequence = frame.sequence;
      result.sessionId = frame.sessionId;

      const bool sequenceOk = observeSequence(frame);
      ++metrics_.consumedFrames;
      if (result.consumedFrames == 1U) observeDispatchInterval(nowMs);
      if (!sequenceOk) {
        result.ok = false;
        result.sequenceIncomplete = true;
        ++metrics_.sequenceFailures;
      }

      audio.observeCapturedMono(frame.samples, kAudioCaptureSamplesPerFrame);
      const RecorderOperationOwner recorderOwner = recorder.operationOwner();
      const AudioCaptureDispatchTarget target = audioCaptureDispatchTarget(
          router.owner(), recorderOwner, recorder.recording(),
          voice.acceptingAudio());

      if (target == AudioCaptureDispatchTarget::none) {
        result.ok = false;
        result.routingFailure = true;
        ++metrics_.routingFailures;
        continue;
      }

      if (target == AudioCaptureDispatchTarget::bleVoice) {
        if (!sequenceOk ||
            !voice.appendMono16(frame.samples, kAudioCaptureSamplesPerFrame,
                                nowMs)) {
          result.ok = false;
          result.voiceDeliveryFailure = true;
          ++metrics_.voiceDeliveryFailures;
        }
        continue;
      }

      if (!sequenceOk) {
        (void)recorder.reportCaptureFailure(
            log, RecorderTerminal::captureFailure,
            RecorderFailureStage::captureIncomplete);
      }
      if (recorder.captureFailureLatched() ||
          !recorder.appendMono16(frame.samples,
                                 kAudioCaptureSamplesPerFrame, log)) {
        // appendMono16() latches queue overflow itself. The explicit report is
        // a fail-safe for every other false return and is first-failure-wins,
        // so it cannot overwrite the more precise overflow/short-write stage.
        (void)recorder.reportCaptureFailure(
            log, RecorderTerminal::captureFailure,
            RecorderFailureStage::captureIncomplete);
        result.ok = false;
        result.recorderDeliveryFailure = true;
        result.failedRecorderOwner = recorderOwner;
        ++metrics_.recorderDeliveryFailures;
      }
    }
    return result;
  }

  void reset() {
    sessionId_ = 0;
    lastSequence_ = 0;
    sequenceObserved_ = false;
    metrics_ = {};
    lastDispatchMs_ = 0;
    dispatchIntervalObserved_ = false;
  }

  const AudioCaptureDispatcherMetrics &metrics() const { return metrics_; }

 private:
  static size_t intervalBucket(uint32_t elapsedUs) {
    static constexpr uint32_t limits[
        AudioCaptureDispatcherMetrics::kIntervalHistogramBuckets] = {
        250U, 500U, 1000U, 2000U, 4000U, 8000U,
        16000U, 32000U, 64000U, 128000U, 512000U, UINT32_MAX};
    for (size_t i = 0;
         i < AudioCaptureDispatcherMetrics::kIntervalHistogramBuckets; ++i) {
      if (elapsedUs <= limits[i]) return i;
    }
    return AudioCaptureDispatcherMetrics::kIntervalHistogramBuckets - 1U;
  }

  void observeDispatchInterval(uint32_t nowMs) {
    // The metric is the gap between drain calls that actually delivered at
    // least one frame. Empty loop polls carry no audio work and are excluded.
    if (dispatchIntervalObserved_) {
      const uint32_t elapsedUs = (nowMs - lastDispatchMs_) * 1000U;
      if (elapsedUs > metrics_.maximumIntervalUs) {
        metrics_.maximumIntervalUs = elapsedUs;
      }
      ++metrics_.intervalSamples;
      ++metrics_.intervalHistogram[intervalBucket(elapsedUs)];
    }
    lastDispatchMs_ = nowMs;
    dispatchIntervalObserved_ = true;
  }

  bool observeSequence(const AudioCaptureFrame &frame) {
    if (frame.sessionId == 0) return false;
    if (frame.sessionId != sessionId_) {
      sessionId_ = frame.sessionId;
      lastSequence_ = frame.sequence;
      sequenceObserved_ = true;
      metrics_ = {};
      metrics_.sessionId = frame.sessionId;
      dispatchIntervalObserved_ = false;
      return true;
    }
    if (!sequenceObserved_) {
      lastSequence_ = frame.sequence;
      sequenceObserved_ = true;
      return true;
    }
    const bool contiguous = frame.sequence == lastSequence_ + 1U;
    lastSequence_ = frame.sequence;
    return contiguous;
  }

  uint32_t sessionId_ = 0;
  uint32_t lastSequence_ = 0;
  bool sequenceObserved_ = false;
  uint32_t lastDispatchMs_ = 0;
  bool dispatchIntervalObserved_ = false;
  AudioCaptureDispatcherMetrics metrics_;
};

}  // namespace pokepod
