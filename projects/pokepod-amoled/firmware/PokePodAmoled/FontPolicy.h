#pragma once

#include <stdint.h>

namespace pokepod {

constexpr uint64_t kFontHeaderBytes = 20;
constexpr uint8_t kFontBitsPerPixel = 4;
constexpr uint32_t kMaximumFontGlyphs = 65536;

constexpr uint32_t fontBitmapBytes(uint16_t width, uint16_t height) {
  return (static_cast<uint32_t>(width) * height + 1U) / 2U;
}

constexpr uint32_t fontEntryBytes(uint16_t width, uint16_t height) {
  return 8U + fontBitmapBytes(width, height);
}

inline bool validFontLayout(uint16_t width, uint16_t height,
                            uint8_t bitsPerPixel, uint32_t glyphCount,
                            uint32_t entrySize, uint64_t fileSize) {
  if (width != 20 || height != 20 || bitsPerPixel != kFontBitsPerPixel ||
      glyphCount == 0 || glyphCount > kMaximumFontGlyphs ||
      entrySize != fontEntryBytes(width, height)) {
    return false;
  }
  const uint64_t expected = kFontHeaderBytes +
      static_cast<uint64_t>(glyphCount) * entrySize;
  return expected == fileSize;
}

inline uint8_t alpha4At(const uint8_t *bitmap, uint32_t pixelIndex) {
  const uint8_t packed = bitmap[pixelIndex / 2];
  return (pixelIndex & 1U) == 0 ? packed >> 4 : packed & 0x0F;
}

inline uint16_t blendRgb565(uint16_t foreground, uint16_t background,
                            uint8_t alpha4) {
  if (alpha4 == 0) return background;
  if (alpha4 >= 15) return foreground;
  const uint16_t inverse = 15 - alpha4;
  const uint16_t red = (((foreground >> 11) & 0x1F) * alpha4 +
                        ((background >> 11) & 0x1F) * inverse + 7) / 15;
  const uint16_t green = (((foreground >> 5) & 0x3F) * alpha4 +
                          ((background >> 5) & 0x3F) * inverse + 7) / 15;
  const uint16_t blue = ((foreground & 0x1F) * alpha4 +
                         (background & 0x1F) * inverse + 7) / 15;
  return static_cast<uint16_t>((red << 11) | (green << 5) | blue);
}

}  // namespace pokepod
