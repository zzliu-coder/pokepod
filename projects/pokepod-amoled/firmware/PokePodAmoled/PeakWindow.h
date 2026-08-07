#pragma once

#include <stddef.h>
#include <stdint.h>

namespace pokepod {

class PeakWindow {
 public:
  static constexpr size_t kEnvelopeSamples = 24;
  static constexpr uint8_t kReadsPerEnvelopeSample = 4;

  void observe(uint16_t sample) {
    latest_ = sample;
    if (sample > window_) window_ = sample;
    if (sample > bucketPeak_) bucketPeak_ = sample;
    if (++bucketReads_ >= kReadsPerEnvelopeSample) {
      envelope_[envelopeWrite_] = bucketPeak_;
      envelopeWrite_ = (envelopeWrite_ + 1) % kEnvelopeSamples;
      if (envelopeCount_ < kEnvelopeSamples) ++envelopeCount_;
      bucketReads_ = 0;
      bucketPeak_ = 0;
    }
  }

  uint16_t consume() {
    const uint16_t value = window_;
    window_ = 0;
    return value;
  }

  void reset() {
    latest_ = 0;
    window_ = 0;
    bucketPeak_ = 0;
    bucketReads_ = 0;
    envelopeWrite_ = 0;
    envelopeCount_ = 0;
    for (size_t index = 0; index < kEnvelopeSamples; ++index) {
      envelope_[index] = 0;
    }
  }

  uint16_t latest() const { return latest_; }

  void copyEnvelope(uint16_t *output, size_t count) const {
    if (output == nullptr) return;
    const size_t requested = count < kEnvelopeSamples
        ? count : kEnvelopeSamples;
    const size_t padding = requested > envelopeCount_
        ? requested - envelopeCount_ : 0;
    for (size_t index = 0; index < padding; ++index) output[index] = 0;
    const size_t available = requested - padding;
    const size_t first = (envelopeWrite_ + kEnvelopeSamples - available) %
        kEnvelopeSamples;
    for (size_t index = 0; index < available; ++index) {
      output[padding + index] = envelope_[(first + index) % kEnvelopeSamples];
    }
  }

 private:
  uint16_t latest_ = 0;
  uint16_t window_ = 0;
  uint16_t bucketPeak_ = 0;
  uint8_t bucketReads_ = 0;
  uint16_t envelope_[kEnvelopeSamples] = {};
  size_t envelopeWrite_ = 0;
  size_t envelopeCount_ = 0;
};

}  // namespace pokepod
