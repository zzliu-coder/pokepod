#pragma once

#include <stdint.h>

namespace pokepod {

constexpr uint64_t kFontHeaderBytes = 16;
constexpr uint32_t kFontEntryBytes = 36;
constexpr uint32_t kMaximumFontGlyphs = 65536;

inline bool validFontLayout(uint16_t width, uint16_t height,
                            uint32_t glyphCount, uint32_t entrySize,
                            uint64_t fileSize) {
  if (width != 16 || height != 16 || glyphCount == 0 ||
      glyphCount > kMaximumFontGlyphs || entrySize != kFontEntryBytes) {
    return false;
  }
  const uint64_t expected = kFontHeaderBytes +
      static_cast<uint64_t>(glyphCount) * entrySize;
  return expected == fileSize;
}

}  // namespace pokepod
