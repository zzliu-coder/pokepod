#pragma once

#include <stddef.h>
#include <stdint.h>

namespace pokepod {

// Linear-phase 16 kHz FIR that removes the stable 5-7 kHz microphone hiss
// measured on PokePod V1 while preserving the speech-presence band.  The
// coefficients are Q15, symmetric, and sum to exactly 32768 (unity at DC).
//
// Measured response after Q15 quantisation:
//   4.8 kHz: -0.2 dB
//   5.3 kHz: -32 dB
//   5.5-6.5 kHz: at least -46 dB at the centre and -32 dB at the edges
//   7.2 kHz: -0.2 dB
//
// This stage deliberately has no gate or gain policy.  VoiceConditioner owns
// those dynamics so tonal preservation and noise suppression stay independent.
class TargetedHissFilter {
 public:
  static constexpr size_t kTaps = 67;

  void reset() {
    index_ = 0;
    for (auto &sample : ring_) sample = 0;
  }

  int32_t process(int32_t sample) {
    ring_[index_] = sample;
    index_ = (index_ + 1) % kTaps;

    int64_t accumulator = 0;
    size_t ringIndex = index_;
    for (size_t tap = 0; tap < kTaps; ++tap) {
      accumulator += static_cast<int64_t>(ring_[ringIndex]) * kFirQ15[tap];
      ringIndex = (ringIndex + 1) % kTaps;
    }
    return static_cast<int32_t>(
        (accumulator + (accumulator >= 0 ? 16384 : -16384)) / 32768);
  }

 private:
  // 5.05-6.95 kHz ideal band-stop, 67 taps, Kaiser beta 3.5.  The wider
  // design band keeps the measured 5.31-6.3 kHz cluster inside the deep stop
  // region while the finite transition remains essentially flat at 4.8 kHz.
  static constexpr int16_t kFirQ15[kTaps] = {
      -15, 62, -76, 0, 124, -177, 103, 0,
      22, -163, 215, 0, -366, 536, -331, 0,
      34, 280, -454, 0, 888, -1384, 931, 0,
      -314, -374, 997, 0, -2728, 5087, -4370, 0,
      5368, 24978, 5368, 0, -4370, 5087, -2728, 0,
      997, -374, -314, 0, 931, -1384, 888, 0,
      -454, 280, 34, 0, -331, 536, -366, 0,
      215, -163, 22, 0, 103, -177, 124, 0,
      -76, 62, -15};

  int32_t ring_[kTaps] = {};
  size_t index_ = 0;
};

}  // namespace pokepod
