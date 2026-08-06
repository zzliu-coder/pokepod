#include <cassert>
#include <cstdint>
#include <vector>

#include "../PokePodAmoled/AudioDecimator.h"

int main() {
  using namespace pokepod;
  AudioDecimator3 decimator;
  std::vector<uint8_t> stereo(480 * 4);
  for (size_t frame = 0; frame < 480; ++frame) {
    const int16_t left = 12000;
    const int16_t right = -7000;
    stereo[frame * 4] = static_cast<uint8_t>(left & 0xff);
    stereo[frame * 4 + 1] = static_cast<uint8_t>((left >> 8) & 0xff);
    stereo[frame * 4 + 2] = static_cast<uint8_t>(right & 0xff);
    stereo[frame * 4 + 3] = static_cast<uint8_t>((right >> 8) & 0xff);
  }
  std::vector<uint8_t> mono(480 * 2);
  const size_t bytes = decimator.processStereo16(
      stereo.data(), stereo.size(), mono.data(), mono.size());
  assert(bytes >= 312 && bytes <= 320);
  for (size_t offset = 0; offset + 1 < bytes; offset += 2) {
    const int16_t sample = static_cast<int16_t>(
        static_cast<uint16_t>(mono[offset]) |
        (static_cast<uint16_t>(mono[offset + 1]) << 8));
    assert(sample == 12000);
  }

  decimator.reset();
  assert(decimator.processStereo16(nullptr, stereo.size(), mono.data(), mono.size()) == 0);
  assert(decimator.processStereo16(stereo.data(), stereo.size(), nullptr, mono.size()) == 0);
  return 0;
}
