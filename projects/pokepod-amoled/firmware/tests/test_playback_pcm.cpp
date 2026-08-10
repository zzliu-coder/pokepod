#include <assert.h>
#include <stdint.h>

#include "../PokePodAmoled/PlaybackPcm.h"

using namespace pokepod;

int main() {
  const uint8_t mono[] = {0x34, 0x12, 0x00, 0x80, 0xff, 0x7f};
  uint8_t stereo[12] = {};
  assert(mono16LittleEndianToStereo16LittleEndian(
             mono, sizeof(mono), stereo, sizeof(stereo)) == sizeof(stereo));
  const uint8_t expected[] = {
      0x34, 0x12, 0x34, 0x12,
      0x00, 0x80, 0x00, 0x80,
      0xff, 0x7f, 0xff, 0x7f,
  };
  for (size_t i = 0; i < sizeof(expected); ++i) {
    assert(stereo[i] == expected[i]);
  }

  uint8_t tooSmall[11] = {};
  assert(mono16LittleEndianToStereo16LittleEndian(
             mono, sizeof(mono), tooSmall, sizeof(tooSmall)) == 0);
  assert(mono16LittleEndianToStereo16LittleEndian(
             mono, sizeof(mono) - 1, stereo, sizeof(stereo)) == 0);
  return 0;
}
