#pragma once

#include <stddef.h>
#include <stdint.h>

namespace pokepod {

// Capsule WAV files are 16 kHz, 16-bit, mono.  The ES8311 board output is
// wired as 16-bit stereo, so duplicate each little-endian sample into both
// slots without changing the sample rate.
inline size_t mono16LittleEndianToStereo16LittleEndian(
    const uint8_t *input, size_t inputBytes, uint8_t *output,
    size_t outputCapacity) {
  if (input == nullptr || output == nullptr || (inputBytes % 2) != 0 ||
      outputCapacity < inputBytes * 2) {
    return 0;
  }
  size_t written = 0;
  for (size_t offset = 0; offset < inputBytes; offset += 2) {
    output[written++] = input[offset];
    output[written++] = input[offset + 1];
    output[written++] = input[offset];
    output[written++] = input[offset + 1];
  }
  return written;
}

}  // namespace pokepod
