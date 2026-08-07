#include <cassert>
#include <cstdint>

#include "../PokePodAmoled/FontPolicy.h"

int main() {
  using namespace pokepod;
  static_assert(kFontHeaderBytes == 20, "PKF2 header is stable");
  static_assert(fontBitmapBytes(20, 20) == 200, "A4 bitmap size");
  static_assert(fontEntryBytes(20, 20) == 208, "PKF2 entry size");
  const uint64_t validSize = kFontHeaderBytes + 123ULL * 208ULL;
  assert(validFontLayout(20, 20, 4, 123, 208, validSize));
  assert(!validFontLayout(16, 16, 4, 123, 136,
                          kFontHeaderBytes + 123ULL * 136ULL));
  assert(!validFontLayout(20, 20, 1, 123, 208, validSize));
  assert(!validFontLayout(20, 20, 4, 0, 208, kFontHeaderBytes));
  assert(!validFontLayout(20, 20, 4, 123, 207, validSize));
  assert(!validFontLayout(20, 20, 4, 123, 208, validSize - 1));

  const uint8_t packed[] = {0x0F, 0x81};
  assert(alpha4At(packed, 0) == 0);
  assert(alpha4At(packed, 1) == 15);
  assert(alpha4At(packed, 2) == 8);
  assert(alpha4At(packed, 3) == 1);
  assert(blendRgb565(0xFFFF, 0x0000, 0) == 0x0000);
  assert(blendRgb565(0xFFFF, 0x0000, 15) == 0xFFFF);
  const uint16_t half = blendRgb565(0xFFFF, 0x0000, 8);
  assert(((half >> 11) & 0x1F) >= 16);
  assert(((half >> 5) & 0x3F) >= 32);
  return 0;
}
