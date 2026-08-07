#include "ChineseRenderer.h"

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

template <typename Glyph>
bool findFixedGlyph(const Glyph *glyphs, uint32_t count, uint32_t codepoint,
                    uint8_t &advance, uint8_t *bitmap,
                    uint16_t bitmapBytes) {
  uint32_t low = 0;
  uint32_t high = count;
  while (low < high) {
    const uint32_t middle = low + (high - low) / 2;
    if (glyphs[middle].codepoint < codepoint) low = middle + 1;
    else high = middle;
  }
  if (low >= count || glyphs[low].codepoint != codepoint) return false;
  advance = glyphs[low].advance;
  memcpy(bitmap, glyphs[low].bitmap, bitmapBytes);
  return true;
}

template <typename Glyph>
uint8_t findFixedAdvance(const Glyph *glyphs, uint32_t count,
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
                               UiTextSize size, bool bold) {
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

    GlyphData glyph;
    const bool found =
        (preferSdFont && loadSd(codepoint, size, glyph)) ||
        loadFixed(codepoint, size, glyph) ||
        (!preferSdFont && loadSd(codepoint, size, glyph));
    const uint8_t width = found ? glyph.advance : glyphPixels(size);
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
    const int16_t drawY = y +
        (logicalLine - skipLines) * lineHeight(size);
    if (found) {
      drawGlyph(x + cursorX, drawY, glyph, color, background, bold);
    } else {
      const uint8_t pixels = glyphPixels(size);
      display_->fillRect(x + cursorX, drawY, width, pixels, background);
      display_->drawRect(x + cursorX + 1, drawY + 1,
                         width - 2, pixels - 2, color);
    }
    cursorX += width;
  }
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

bool ChineseRenderer::loadFixed(uint32_t codepoint, UiTextSize size,
                                GlyphData &glyph) const {
  glyph.pixels = glyphPixels(size);
  const uint16_t bytes = fontBitmapBytes(glyph.pixels, glyph.pixels);
  switch (size) {
    case UiTextSize::compact:
      return findFixedGlyph(kFixedGlyphs16, kFixedGlyphs16Count, codepoint,
                            glyph.advance, glyph.bitmap, bytes);
    case UiTextSize::body:
      return findFixedGlyph(kFixedGlyphs20, kFixedGlyphs20Count, codepoint,
                            glyph.advance, glyph.bitmap, bytes);
    case UiTextSize::display:
      return findFixedGlyph(kFixedGlyphs28, kFixedGlyphs28Count, codepoint,
                            glyph.advance, glyph.bitmap, bytes);
    case UiTextSize::timer:
      return findFixedGlyph(kFixedGlyphs36, kFixedGlyphs36Count, codepoint,
                            glyph.advance, glyph.bitmap, bytes);
  }
  return false;
}

bool ChineseRenderer::loadSd(uint32_t codepoint, UiTextSize size,
                             GlyphData &glyph) {
  if (size != UiTextSize::body || !sdFontReady_ || !fontFile_) return false;
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
  return fontFile_.read(glyph.bitmap, fontBitmapBytes(20, 20)) ==
      fontBitmapBytes(20, 20);
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
                                uint16_t background, bool bold) {
  display_->fillRect(x, y, glyph.advance, glyph.pixels, background);
  for (uint16_t row = 0; row < glyph.pixels; ++row) {
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
