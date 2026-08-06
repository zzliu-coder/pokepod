#pragma once

#include <stddef.h>
#include <stdint.h>

namespace pokepod {

// The ES8311 produces 48 kHz stereo slots while only the left slot carries the
// physical microphone used by USB. A nine-tap symmetric low-pass filter is
// followed by decimation by three, yielding 16 kHz mono PCM for capsules.
class AudioDecimator3 {
 public:
  void reset() {
    sampleCount_ = 0;
    phase_ = 0;
    ringIndex_ = 0;
    for (auto &sample : ring_) sample = 0;
  }

  size_t processStereo16(const uint8_t *stereo, size_t stereoBytes,
                         uint8_t *mono, size_t monoCapacity) {
    if (stereo == nullptr || mono == nullptr) return 0;
    const size_t frames = stereoBytes / 4;
    size_t outputBytes = 0;
    for (size_t frame = 0; frame < frames; ++frame) {
      const uint16_t raw = static_cast<uint16_t>(stereo[frame * 4]) |
                           (static_cast<uint16_t>(stereo[frame * 4 + 1]) << 8);
      const int16_t sample = static_cast<int16_t>(raw);
      ring_[ringIndex_] = sample;
      ringIndex_ = (ringIndex_ + 1) % kTaps;
      ++sampleCount_;
      phase_ = static_cast<uint8_t>((phase_ + 1) % 3);
      if (sampleCount_ < kTaps || phase_ != 0 || outputBytes + 2 > monoCapacity) continue;

      int32_t filtered = 0;
      size_t index = ringIndex_;
      for (size_t tap = 0; tap < kTaps; ++tap) {
        filtered += static_cast<int32_t>(ring_[index]) * kCoefficients[tap];
        index = (index + 1) % kTaps;
      }
      filtered = (filtered + (filtered >= 0 ? 32 : -32)) / 64;
      if (filtered > 32767) filtered = 32767;
      if (filtered < -32768) filtered = -32768;
      const uint16_t encoded = static_cast<uint16_t>(static_cast<int16_t>(filtered));
      mono[outputBytes++] = static_cast<uint8_t>(encoded & 0xff);
      mono[outputBytes++] = static_cast<uint8_t>((encoded >> 8) & 0xff);
    }
    return outputBytes;
  }

 private:
  static constexpr size_t kTaps = 9;
  static constexpr int16_t kCoefficients[kTaps] = {2, 4, 8, 12, 12, 12, 8, 4, 2};
  int16_t ring_[kTaps] = {};
  uint64_t sampleCount_ = 0;
  uint8_t phase_ = 0;
  size_t ringIndex_ = 0;
};

}  // namespace pokepod
