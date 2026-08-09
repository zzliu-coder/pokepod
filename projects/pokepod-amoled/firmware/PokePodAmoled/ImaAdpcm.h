#pragma once

#include <stddef.h>
#include <stdint.h>

namespace pokepod {

struct ImaAdpcmState {
  int16_t predictor = 0;
  uint8_t stepIndex = 0;
};

constexpr int16_t clampAdpcmSample(int32_t value) {
  return value > 32767 ? 32767 : (value < -32768 ? -32768
                                                  : static_cast<int16_t>(value));
}

constexpr uint8_t clampAdpcmIndex(int value) {
  return value < 0 ? 0 : (value > 88 ? 88 : static_cast<uint8_t>(value));
}

inline uint8_t imaAdpcmEncodeSample(int16_t sample, ImaAdpcmState &state) {
  static constexpr int16_t steps[89] = {
      7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
      34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130,
      143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449,
      494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411,
      1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026,
      4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
      11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
      27086, 29794, 32767};
  static constexpr int8_t indexAdjust[16] = {
      -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

  const int step = steps[state.stepIndex];
  int difference = static_cast<int>(sample) - state.predictor;
  uint8_t code = 0;
  if (difference < 0) {
    code = 8;
    difference = -difference;
  }
  int delta = step >> 3;
  if (difference >= step) {
    code |= 4;
    difference -= step;
    delta += step;
  }
  if (difference >= (step >> 1)) {
    code |= 2;
    difference -= step >> 1;
    delta += step >> 1;
  }
  if (difference >= (step >> 2)) {
    code |= 1;
    delta += step >> 2;
  }
  state.predictor = clampAdpcmSample(
      static_cast<int32_t>(state.predictor) + ((code & 8) ? -delta : delta));
  state.stepIndex = clampAdpcmIndex(
      static_cast<int>(state.stepIndex) + indexAdjust[code]);
  return code;
}

inline int16_t imaAdpcmDecodeSample(uint8_t code, ImaAdpcmState &state) {
  static constexpr int16_t steps[89] = {
      7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
      34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130,
      143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449,
      494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411,
      1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026,
      4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
      11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
      27086, 29794, 32767};
  static constexpr int8_t indexAdjust[16] = {
      -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};
  code &= 0x0f;
  const int step = steps[state.stepIndex];
  int delta = step >> 3;
  if (code & 4) delta += step;
  if (code & 2) delta += step >> 1;
  if (code & 1) delta += step >> 2;
  state.predictor = clampAdpcmSample(
      static_cast<int32_t>(state.predictor) + ((code & 8) ? -delta : delta));
  state.stepIndex = clampAdpcmIndex(
      static_cast<int>(state.stepIndex) + indexAdjust[code]);
  return state.predictor;
}

// Each BLE frame seeds its own predictor and step index. The first PCM sample
// is carried by the frame header, so a lost frame cannot corrupt the next one.
inline size_t imaAdpcmEncodeIndependent(const int16_t *samples,
                                        size_t sampleCount,
                                        uint8_t *encoded,
                                        size_t capacity,
                                        ImaAdpcmState &initial) {
  if (samples == nullptr || encoded == nullptr || sampleCount == 0) return 0;
  const size_t required = sampleCount / 2;
  if (capacity < required) return 0;
  initial.predictor = samples[0];
  initial.stepIndex = 0;
  ImaAdpcmState state = initial;
  size_t output = 0;
  uint8_t packed = 0;
  bool lowNibble = true;
  for (size_t index = 1; index < sampleCount; ++index) {
    const uint8_t code = imaAdpcmEncodeSample(samples[index], state);
    if (lowNibble) {
      packed = code;
      lowNibble = false;
    } else {
      encoded[output++] = static_cast<uint8_t>(packed | (code << 4));
      lowNibble = true;
    }
  }
  if (!lowNibble) encoded[output++] = packed;
  return output;
}

inline size_t imaAdpcmDecodeIndependent(const uint8_t *encoded,
                                        size_t encodedBytes,
                                        ImaAdpcmState initial,
                                        int16_t *samples,
                                        size_t sampleCount) {
  if (encoded == nullptr || samples == nullptr || sampleCount == 0 ||
      encodedBytes < sampleCount / 2) return 0;
  samples[0] = initial.predictor;
  ImaAdpcmState state = initial;
  for (size_t index = 1; index < sampleCount; ++index) {
    const uint8_t packed = encoded[(index - 1) / 2];
    const uint8_t code = ((index - 1) & 1) == 0 ? packed & 0x0f : packed >> 4;
    samples[index] = imaAdpcmDecodeSample(code, state);
  }
  return sampleCount;
}

}  // namespace pokepod
