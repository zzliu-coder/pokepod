#pragma once

#include <stddef.h>
#include <stdint.h>

#include "TargetedHissFilter.h"

namespace pokepod {

struct VoiceConditionerMetrics {
  uint64_t outputSamples = 0;
  uint32_t gatedSamples = 0;
  uint32_t suppressedSamples = 0;
  uint32_t limitedSamples = 0;
  uint16_t outputPeak = 0;
  uint16_t estimatedNoiseFloor = 0;
  uint32_t maximumGainQ12 = 4096;
};

// Deterministic 16 kHz voice conditioning shared by capsule and BLE capture.
// The chain is deliberately small: DC/high-pass cleanup, a targeted hiss
// rejector, an adaptive noise gate, restrained AGC and a final limiter. It owns
// no heap memory and can be tested independently from I2S and the decimator.
class VoiceConditioner {
 public:
  static constexpr int32_t kLimiter = 30000;
  static constexpr int32_t kInitialGainQ12 = 2 * 4096;
  static constexpr int32_t kMaximumGainQ12 = 6 * 4096;

  void reset(VoiceConditionerMetrics *metrics) {
    metrics_ = metrics;
    previousHighPassInput_ = 0;
    previousHighPassOutput_ = 0;
    hissFilter_.reset();
    envelopeQ8_ = 0;
    noiseFloorQ8_ = kInitialNoiseFloor << 8;
    gateOpen_ = false;
    gateHangoverSamples_ = 0;
    gateGainQ12_ = kClosedGateGainQ12;
    gainQ12_ = kInitialGainQ12;
    if (metrics_ != nullptr) {
      *metrics_ = {};
      metrics_->maximumGainQ12 = static_cast<uint32_t>(gainQ12_);
      metrics_->estimatedNoiseFloor = kInitialNoiseFloor;
    }
  }

  int32_t process(int32_t sample) {
    const int32_t highPassed = highPass(sample);
    const int32_t cleaned = hissFilter_.process(highPassed);
    const int32_t absolute = magnitude(cleaned);

    const int32_t envelopeTarget = absolute << 8;
    envelopeQ8_ = approach(envelopeQ8_, envelopeTarget,
                           envelopeTarget > envelopeQ8_ ? 4 : 10);
    const int32_t envelope = envelopeQ8_ >> 8;

    updateNoiseFloor(absolute, envelope);
    const int32_t noiseFloor = noiseFloorQ8_ >> 8;
    const int32_t openLevel = maximum(kMinimumGateOpenLevel, noiseFloor * 3);
    const int32_t closeLevel = maximum(kMinimumGateCloseLevel, noiseFloor * 2);
    if (envelope >= openLevel) {
      gateOpen_ = true;
      gateHangoverSamples_ = kGateHangoverSamples;
    } else if (gateOpen_) {
      if (envelope > closeLevel) {
        gateHangoverSamples_ = kGateHangoverSamples;
      } else if (gateHangoverSamples_ > 0) {
        --gateHangoverSamples_;
      } else {
        gateOpen_ = false;
      }
    }

    const int32_t gateTarget = gateOpen_ ? kOpenGateGainQ12
                                         : kClosedGateGainQ12;
    gateGainQ12_ = approach(gateGainQ12_, gateTarget,
                            gateOpen_ ? 3 : 6);
    if (!gateOpen_ && metrics_ != nullptr) ++metrics_->gatedSamples;
    if (gateGainQ12_ < kHalfOpenGateGainQ12 && metrics_ != nullptr) {
      ++metrics_->suppressedSamples;
    }

    int32_t targetGain = kInitialGainQ12;
    if (gateOpen_ && envelope > 0) {
      targetGain = static_cast<int32_t>(
          static_cast<int64_t>(kTargetEnvelope) * 4096 / envelope);
      targetGain = clamp(targetGain, 4096, kMaximumGainQ12);
    }
    gainQ12_ = approach(gainQ12_, targetGain,
                        targetGain < gainQ12_ ? 3 : 10);
    if (metrics_ != nullptr &&
        static_cast<uint32_t>(gainQ12_) > metrics_->maximumGainQ12) {
      metrics_->maximumGainQ12 = static_cast<uint32_t>(gainQ12_);
    }

    int64_t scaled = static_cast<int64_t>(cleaned) * gainQ12_ / 4096;
    scaled = scaled * gateGainQ12_ / 4096;
    if (scaled > kLimiter) {
      scaled = kLimiter;
      if (metrics_ != nullptr) ++metrics_->limitedSamples;
    } else if (scaled < -kLimiter) {
      scaled = -kLimiter;
      if (metrics_ != nullptr) ++metrics_->limitedSamples;
    }

    if (metrics_ != nullptr) {
      const uint16_t outputMagnitude = static_cast<uint16_t>(
          scaled < 0 ? -scaled : scaled);
      if (outputMagnitude > metrics_->outputPeak) {
        metrics_->outputPeak = outputMagnitude;
      }
      metrics_->estimatedNoiseFloor = static_cast<uint16_t>(noiseFloor);
      ++metrics_->outputSamples;
    }
    return static_cast<int32_t>(scaled);
  }

 private:
  static constexpr int32_t kHighPassFeedbackQ15 = 31690;
  // Real V1 microphones can deliver ordinary speech with a post-FIR envelope
  // below 96.  Keep the adaptive threshold close to the measured noise floor
  // and use a soft closed gain so quiet speech can open the gate instead of
  // being quantized to an all-zero WAV.
  static constexpr int32_t kInitialNoiseFloor = 8;
  static constexpr int32_t kMinimumNoiseFloor = 8;
  static constexpr int32_t kMaximumNoiseFloor = 512;
  static constexpr int32_t kMinimumGateOpenLevel = 40;
  static constexpr int32_t kMinimumGateCloseLevel = 20;
  static constexpr int32_t kClosedGateGainQ12 = 128;
  static constexpr int32_t kHalfOpenGateGainQ12 = 2048;
  static constexpr int32_t kOpenGateGainQ12 = 4096;
  static constexpr int32_t kTargetEnvelope = 4000;
  static constexpr uint16_t kGateHangoverSamples = 1600;  // 100 ms at 16 kHz

  static int32_t maximum(int32_t left, int32_t right) {
    return left > right ? left : right;
  }

  static int32_t clamp(int32_t value, int32_t minimum, int32_t maximumValue) {
    if (value < minimum) return minimum;
    if (value > maximumValue) return maximumValue;
    return value;
  }

  static int32_t magnitude(int32_t value) {
    return value < 0 ? -value : value;
  }

  static int32_t approach(int32_t current, int32_t target, uint8_t shift) {
    const int32_t delta = target - current;
    if (delta == 0) return current;
    int32_t step = delta >> shift;
    if (step == 0) step = delta > 0 ? 1 : -1;
    return current + step;
  }

  int32_t highPass(int32_t sample) {
    const int32_t output = sample - previousHighPassInput_ +
        static_cast<int32_t>(
            static_cast<int64_t>(kHighPassFeedbackQ15) *
            previousHighPassOutput_ / 32768);
    previousHighPassInput_ = sample;
    previousHighPassOutput_ = output;
    return output;
  }

  void updateNoiseFloor(int32_t absolute, int32_t envelope) {
    const int32_t current = noiseFloorQ8_ >> 8;
    const int32_t trackingCeiling = maximum(kMinimumGateOpenLevel,
                                             current * 4);
    if (!gateOpen_ && envelope < trackingCeiling) {
      const int32_t target = clamp(absolute, kMinimumNoiseFloor,
                                   kMaximumNoiseFloor) << 8;
      noiseFloorQ8_ = approach(noiseFloorQ8_, target,
                               target < noiseFloorQ8_ ? 6 : 11);
      noiseFloorQ8_ = clamp(noiseFloorQ8_, kMinimumNoiseFloor << 8,
                            kMaximumNoiseFloor << 8);
    }
  }

  VoiceConditionerMetrics *metrics_ = nullptr;
  int32_t previousHighPassInput_ = 0;
  int32_t previousHighPassOutput_ = 0;
  TargetedHissFilter hissFilter_;
  int32_t envelopeQ8_ = 0;
  int32_t noiseFloorQ8_ = kInitialNoiseFloor << 8;
  bool gateOpen_ = false;
  uint16_t gateHangoverSamples_ = 0;
  int32_t gateGainQ12_ = kClosedGateGainQ12;
  int32_t gainQ12_ = kInitialGainQ12;
};

}  // namespace pokepod
