#include "ChineseRenderer.h"

#include <cstdlib>
#include <cstring>
#include <esp_heap_caps.h>

#include "CapsulePolicy.h"
#include "FixedChineseFont.h"
#include "FontPolicy.h"
#include "Utf8Policy.h"

namespace pokepod {
namespace {

uint16_t little16(const uint8_t *data) {
  return static_cast<uint16_t>(data[0]) |
         (static_cast<uint16_t>(data[1]) << 8);
}

uint32_t little32(const uint8_t *data) {
  return static_cast<uint32_t>(data[0]) |
         (static_cast<uint32_t>(data[1]) << 8) |
         (static_cast<uint32_t>(data[2]) << 16) |
         (static_cast<uint32_t>(data[3]) << 24);
}

uint8_t glyphPixels(UiTextSize size) {
  switch (size) {
    case UiTextSize::compact: return 16;
    case UiTextSize::body: return 20;
    case UiTextSize::display: return 28;
    case UiTextSize::timer: return 36;
  }
  return 16;
}

int16_t lineHeight(UiTextSize size) {
  return glyphPixels(size) + (size == UiTextSize::compact ? 5 : 7);
}

bool findFixedGlyph(const FixedGlyphIndex *glyphs, uint32_t count,
                    const uint8_t *data, uint32_t dataBytes,
                    uint32_t codepoint, uint8_t &advance, uint8_t *bitmap,
                    uint16_t bitmapBytes) {
  uint32_t low = 0;
  uint32_t high = count;
  while (low < high) {
    const uint32_t middle = low + (high - low) / 2;
    if (glyphs[middle].codepoint < codepoint) low = middle + 1;
    else high = middle;
  }
  if (low >= count || glyphs[low].codepoint != codepoint) return false;
  const FixedGlyphIndex &glyph = glyphs[low];
  if (glyph.dataOffset > dataBytes ||
      glyph.dataLength > dataBytes - glyph.dataOffset) return false;
  if (!decodeFixedGlyphRle(data + glyph.dataOffset, glyph.dataLength,
                           bitmap, bitmapBytes)) return false;
  advance = glyph.advance;
  return true;
}

uint8_t findFixedAdvance(const FixedGlyphIndex *glyphs, uint32_t count,
                         uint32_t codepoint) {
  uint32_t low = 0;
  uint32_t high = count;
  while (low < high) {
    const uint32_t middle = low + (high - low) / 2;
    if (glyphs[middle].codepoint < codepoint) low = middle + 1;
    else high = middle;
  }
  return low < count && glyphs[low].codepoint == codepoint
      ? glyphs[low].advance : 0;
}

}  // namespace

void ChineseRenderer::begin(Arduino_GFX *display, fs::FS *fs) {
  display_ = display;
  sdFontReady_ = false;
  sdGlyphCount_ = 0;
  sdEntrySize_ = 0;
  sdCacheAge_ = 0;
  std::memset(sdCacheLookup_, 0, sizeof(sdCacheLookup_));
  if (sdCache_ == nullptr) {
    sdCache_ = static_cast<SdGlyphCacheEntry *>(heap_caps_calloc(
        kSdGlyphCacheEntries, sizeof(SdGlyphCacheEntry),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (sdCache_ == nullptr) {
      sdCache_ = static_cast<SdGlyphCacheEntry *>(
          calloc(kSdGlyphCacheEntries, sizeof(SdGlyphCacheEntry)));
    }
  } else {
    std::memset(sdCache_, 0,
                kSdGlyphCacheEntries * sizeof(SdGlyphCacheEntry));
  }
  if (fontFile_) fontFile_.close();
  if (fs == nullptr) return;
  const String path = String(kCapsuleSystem) + "/fonts/cjk20.a4";
  fontFile_ = fs->open(path, FILE_READ);
  uint8_t header[kFontHeaderBytes];
  if (!fontFile_ || fontFile_.isDirectory() ||
      fontFile_.read(header, sizeof(header)) != sizeof(header) ||
      memcmp(header, "PKF2", 4) != 0 ||
      !validFontLayout(little16(header + 4), little16(header + 6), header[8],
                       little32(header + 12), little32(header + 16),
                       fontFile_.size())) {
    if (fontFile_) fontFile_.close();
    return;
  }
  sdGlyphCount_ = little32(header + 12);
  sdEntrySize_ = little32(header + 16);
  sdFontReady_ = true;
}

void ChineseRenderer::drawText(const String &text, int16_t x, int16_t y,
                               int16_t maxWidth, uint8_t maxLines,
                               uint16_t color, uint16_t background,
                               uint16_t skipLines, bool preferSdFont,
                               UiTextSize size, bool bold,
                               int16_t pixelOffset, int16_t clipTop,
                               int16_t clipBottom) {
  if (display_ == nullptr || maxLines == 0 || maxWidth < 4) return;
  size_t offset = 0;
  uint16_t logicalLine = 0;
  int16_t cursorX = 0;
  while (offset < text.length()) {
    const uint32_t codepoint = decodeUtf8(text.c_str(), text.length(), offset);
    if (codepoint == '\r') continue;
    if (codepoint == '\n') {
      ++logicalLine;
      cursorX = 0;
      if (logicalLine >= skipLines + maxLines) break;
      continue;
    }

    const uint8_t fixedWidth = fixedAdvance(codepoint, size);
    uint8_t width = fixedWidth != 0 ? fixedWidth :
        (codepoint < 0x80 ? glyphPixels(size) * 3 / 5 : glyphPixels(size));
    if (cursorX > 0 && cursorX + width > maxWidth) {
      ++logicalLine;
      cursorX = 0;
      if (logicalLine >= skipLines + maxLines) break;
    }
    if (logicalLine < skipLines) {
      cursorX += width;
      continue;
    }
    if (logicalLine >= skipLines + maxLines) break;
    GlyphData glyph;
    const bool found =
        (preferSdFont && loadSd(codepoint, size, glyph)) ||
        loadFixed(codepoint, size, glyph) ||
        (!preferSdFont && loadSd(codepoint, size, glyph));
    if (found) width = glyph.advance;
    const int16_t drawY = y +
        (logicalLine - skipLines) * lineHeight(size) - pixelOffset;
    if (found) {
      drawGlyph(x + cursorX, drawY, glyph, color, background, bold,
                clipTop, clipBottom);
    } else {
      const uint8_t pixels = glyphPixels(size);
      const int16_t visibleTop = drawY < clipTop ? clipTop : drawY;
      const int16_t glyphBottom = drawY + pixels;
      const int16_t visibleBottom =
          glyphBottom > clipBottom ? clipBottom : glyphBottom;
      if (visibleBottom > visibleTop) {
        display_->fillRect(x + cursorX, visibleTop, width,
                           visibleBottom - visibleTop, background);
        if (visibleTop == drawY && visibleBottom == glyphBottom) {
          display_->drawRect(x + cursorX + 1, drawY + 1,
                             width - 2, pixels - 2, color);
        }
      }
    }
    cursorX += width;
  }
}

uint16_t ChineseRenderer::wrappedLineCount(const String &text,
                                           int16_t maxWidth,
                                           UiTextSize size) const {
  if (maxWidth < 4 || text.isEmpty()) return 1;
  size_t offset = 0;
  uint16_t lines = 1;
  int16_t cursorX = 0;
  while (offset < text.length()) {
    const uint32_t codepoint = decodeUtf8(text.c_str(), text.length(), offset);
    if (codepoint == '\r') continue;
    if (codepoint == '\n') {
      ++lines;
      cursorX = 0;
      continue;
    }
    const uint8_t advance = fixedAdvance(codepoint, size);
    const uint8_t width = advance != 0 ? advance :
        (codepoint < 0x80 ? glyphPixels(size) * 3 / 5 : glyphPixels(size));
    if (cursorX > 0 && cursorX + width > maxWidth) {
      ++lines;
      cursorX = 0;
    }
    cursorX += width;
  }
  return lines;
}

int16_t ChineseRenderer::measureTextWidth(const String &text,
                                          UiTextSize size) const {
  size_t offset = 0;
  int16_t current = 0;
  int16_t widest = 0;
  while (offset < text.length()) {
    const uint32_t codepoint = decodeUtf8(text.c_str(), text.length(), offset);
    if (codepoint == '\r') continue;
    if (codepoint == '\n') {
      if (current > widest) widest = current;
      current = 0;
      continue;
    }
    const uint8_t advance = fixedAdvance(codepoint, size);
    current += advance != 0 ? advance :
        (codepoint < 0x80 ? glyphPixels(size) * 3 / 5 : glyphPixels(size));
  }
  return current > widest ? current : widest;
}

int16_t ChineseRenderer::textLineHeight(UiTextSize size) const {
  return lineHeight(size);
}

bool ChineseRenderer::loadFixed(uint32_t codepoint, UiTextSize size,
                                GlyphData &glyph) const {
  glyph.pixels = glyphPixels(size);
  const uint16_t bytes = fontBitmapBytes(glyph.pixels, glyph.pixels);
  switch (size) {
    case UiTextSize::compact:
      return findFixedGlyph(kFixedGlyphs16, kFixedGlyphs16Count,
                            kFixedGlyphData16, kFixedGlyphData16Bytes,
                            codepoint, glyph.advance, glyph.bitmap, bytes);
    case UiTextSize::body:
      return findFixedGlyph(kFixedGlyphs20, kFixedGlyphs20Count,
                            kFixedGlyphData20, kFixedGlyphData20Bytes,
                            codepoint, glyph.advance, glyph.bitmap, bytes);
    case UiTextSize::display:
      return findFixedGlyph(kFixedGlyphs28, kFixedGlyphs28Count,
                            kFixedGlyphData28, kFixedGlyphData28Bytes,
                            codepoint, glyph.advance, glyph.bitmap, bytes);
    case UiTextSize::timer:
      return findFixedGlyph(kFixedGlyphs36, kFixedGlyphs36Count,
                            kFixedGlyphData36, kFixedGlyphData36Bytes,
                            codepoint, glyph.advance, glyph.bitmap, bytes);
  }
  return false;
}

bool ChineseRenderer::loadSd(uint32_t codepoint, UiTextSize size,
                             GlyphData &glyph) {
  if (size != UiTextSize::body || !sdFontReady_ || !fontFile_) return false;
  if (loadSdCache(codepoint, glyph)) return true;
  uint32_t low = 0;
  uint32_t high = sdGlyphCount_;
  uint8_t prefix[8];
  while (low < high) {
    const uint32_t middle = low + (high - low) / 2;
    if (!fontFile_.seek(kFontHeaderBytes + middle * sdEntrySize_) ||
        fontFile_.read(prefix, sizeof(prefix)) != sizeof(prefix)) return false;
    if (little32(prefix) < codepoint) low = middle + 1;
    else high = middle;
  }
  if (low >= sdGlyphCount_ ||
      !fontFile_.seek(kFontHeaderBytes + low * sdEntrySize_) ||
      fontFile_.read(prefix, sizeof(prefix)) != sizeof(prefix) ||
      little32(prefix) != codepoint) return false;
  glyph.pixels = 20;
  glyph.advance = prefix[4];
  const bool loaded = fontFile_.read(glyph.bitmap, fontBitmapBytes(20, 20)) ==
      fontBitmapBytes(20, 20);
  if (loaded) storeSdCache(codepoint, glyph);
  return loaded;
}

bool ChineseRenderer::loadSdCache(uint32_t codepoint, GlyphData &glyph) {
  if (sdCache_ == nullptr) return false;
  const uint16_t slot = static_cast<uint16_t>(
      codepoint % kSdGlyphLookupSlots);
  const uint16_t mapped = sdCacheLookup_[slot];
  if (mapped > 0 && mapped <= kSdGlyphCacheEntries) {
    SdGlyphCacheEntry &entry = sdCache_[mapped - 1];
    if (entry.valid && entry.codepoint == codepoint) {
      entry.age = ++sdCacheAge_;
      glyph.pixels = 20;
      glyph.advance = entry.advance;
      std::memcpy(glyph.bitmap, entry.bitmap, kSdGlyphBitmapBytes);
      return true;
    }
  }
  for (uint16_t index = 0; index < kSdGlyphCacheEntries; ++index) {
    SdGlyphCacheEntry &entry = sdCache_[index];
    if (!entry.valid || entry.codepoint != codepoint) continue;
    entry.age = ++sdCacheAge_;
    glyph.pixels = 20;
    glyph.advance = entry.advance;
    std::memcpy(glyph.bitmap, entry.bitmap, kSdGlyphBitmapBytes);
    sdCacheLookup_[slot] = index + 1;
    return true;
  }
  return false;
}

void ChineseRenderer::storeSdCache(uint32_t codepoint,
                                   const GlyphData &glyph) {
  if (sdCache_ == nullptr) return;
  uint16_t target = 0;
  uint32_t oldest = UINT32_MAX;
  for (uint16_t index = 0; index < kSdGlyphCacheEntries; ++index) {
    if (!sdCache_[index].valid) {
      target = index;
      oldest = 0;
      break;
    }
    if (sdCache_[index].age < oldest) {
      oldest = sdCache_[index].age;
      target = index;
    }
  }
  SdGlyphCacheEntry &entry = sdCache_[target];
  entry.codepoint = codepoint;
  entry.age = ++sdCacheAge_;
  entry.advance = glyph.advance;
  entry.valid = true;
  std::memcpy(entry.bitmap, glyph.bitmap, kSdGlyphBitmapBytes);
  sdCacheLookup_[codepoint % kSdGlyphLookupSlots] = target + 1;
}

uint8_t ChineseRenderer::fixedAdvance(uint32_t codepoint,
                                      UiTextSize size) const {
  switch (size) {
    case UiTextSize::compact:
      return findFixedAdvance(kFixedGlyphs16, kFixedGlyphs16Count, codepoint);
    case UiTextSize::body:
      return findFixedAdvance(kFixedGlyphs20, kFixedGlyphs20Count, codepoint);
    case UiTextSize::display:
      return findFixedAdvance(kFixedGlyphs28, kFixedGlyphs28Count, codepoint);
    case UiTextSize::timer:
      return findFixedAdvance(kFixedGlyphs36, kFixedGlyphs36Count, codepoint);
  }
  return 0;
}

void ChineseRenderer::drawGlyph(int16_t x, int16_t y,
                                const GlyphData &glyph, uint16_t color,
                                uint16_t background, bool bold,
                                int16_t clipTop, int16_t clipBottom) {
  const int16_t visibleTop = y < clipTop ? clipTop : y;
  const int16_t glyphBottom = y + glyph.pixels;
  const int16_t visibleBottom =
      glyphBottom > clipBottom ? clipBottom : glyphBottom;
  if (visibleBottom <= visibleTop) return;
  display_->fillRect(x, visibleTop, glyph.advance,
                     visibleBottom - visibleTop, background);
  const uint16_t firstRow = static_cast<uint16_t>(visibleTop - y);
  const uint16_t lastRow = static_cast<uint16_t>(visibleBottom - y);
  for (uint16_t row = firstRow; row < lastRow; ++row) {
    uint16_t column = 0;
    while (column < glyph.advance) {
      uint8_t alpha = alpha4At(
          glyph.bitmap, static_cast<uint32_t>(row) * glyph.pixels + column);
      if (bold && alpha > 0) alpha = alpha > 12 ? 15 : alpha + 3;
      if (alpha == 0) {
        ++column;
        continue;
      }
      const uint16_t runColor = blendRgb565(color, background, alpha);
      const uint16_t start = column++;
      while (column < glyph.advance) {
        uint8_t next = alpha4At(
            glyph.bitmap, static_cast<uint32_t>(row) * glyph.pixels + column);
        if (bold && next > 0) next = next > 12 ? 15 : next + 3;
        if (next != alpha) break;
        ++column;
      }
      display_->fillRect(x + start, y + row, column - start, 1, runColor);
    }
  }
}

}  // namespace pokepod
