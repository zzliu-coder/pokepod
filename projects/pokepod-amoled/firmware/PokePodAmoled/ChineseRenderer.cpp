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
    case UiTextSize::body: return 24;
    case UiTextSize::display: return 32;
    case UiTextSize::compact: return 16;
  }
  return 16;
}

uint8_t asciiTextSize(UiTextSize size) {
  switch (size) {
    case UiTextSize::body: return 3;
    case UiTextSize::display: return 4;
    case UiTextSize::compact: return 2;
  }
  return 2;
}

int16_t glyphWidth(uint32_t codepoint, UiTextSize size) {
  return codepoint < 0x80 ? asciiTextSize(size) * 6 : glyphPixels(size);
}

int16_t lineHeight(UiTextSize size) {
  return glyphPixels(size) + glyphPixels(size) / 4;
}

}  // namespace

void ChineseRenderer::begin(Arduino_GFX *display, fs::FS *fs) {
  display_ = display;
  sdFontReady_ = false;
  sdGlyphCount_ = 0;
  sdEntrySize_ = 0;
  if (fs == nullptr) return;
  const String path = String(kCapsuleSystem) + "/fonts/cjk16.bin";
  fontFile_ = fs->open(path, FILE_READ);
  uint8_t header[16];
  if (!fontFile_ || fontFile_.isDirectory() ||
      fontFile_.read(header, sizeof(header)) != sizeof(header) ||
      memcmp(header, "PKF1", 4) != 0 ||
      !validFontLayout(little16(header + 4), little16(header + 6),
                       little32(header + 8), little32(header + 12),
                       fontFile_.size())) {
    if (fontFile_) fontFile_.close();
    return;
  }
  sdGlyphCount_ = little32(header + 8);
  sdEntrySize_ = little32(header + 12);
  sdFontReady_ = true;
}

void ChineseRenderer::drawText(const String &text, int16_t x, int16_t y,
                               int16_t maxWidth, uint8_t maxLines,
                               uint16_t color, uint16_t background,
                               uint16_t skipLines, bool preferSdFont,
                               UiTextSize size, bool bold) {
  const uint8_t outputPixels = glyphPixels(size);
  if (display_ == nullptr || maxLines == 0 ||
      maxWidth < asciiTextSize(size) * 6) return;
  size_t offset = 0;
  uint16_t logicalLine = 0;
  int16_t cursorX = 0;
  while (offset < text.length()) {
    const uint32_t codepoint = decodeUtf8(text.c_str(), text.length(), offset);
    const int16_t width = glyphWidth(codepoint, size);
    if (codepoint == '\r') continue;
    if (codepoint == '\n' || cursorX + width > maxWidth) {
      ++logicalLine;
      cursorX = 0;
      if (codepoint == '\n') continue;
      if (logicalLine >= skipLines + maxLines) break;
    }
    if (logicalLine < skipLines) {
      cursorX += width;
      continue;
    }
    if (logicalLine >= skipLines + maxLines) break;
    const int16_t drawY =
        y + (logicalLine - skipLines) * lineHeight(size);
    if (codepoint < 0x80) {
      display_->setTextSize(asciiTextSize(size));
      display_->setTextColor(color, background);
      display_->setCursor(x + cursorX, drawY);
      display_->write(static_cast<uint8_t>(codepoint));
      if (bold && codepoint != ' ') {
        display_->setTextColor(color);
        display_->setCursor(x + cursorX + 1, drawY);
        display_->write(static_cast<uint8_t>(codepoint));
      }
    } else {
      uint8_t bitmap[32];
      const bool found = (preferSdFont && loadSd(codepoint, bitmap)) ||
                         loadFixed(codepoint, bitmap) ||
                         (!preferSdFont && loadSd(codepoint, bitmap));
      if (found) {
        drawGlyph(x + cursorX, drawY, bitmap, color, background,
                  outputPixels, bold);
      } else {
        display_->drawRect(x + cursorX + 1, drawY + 1,
                           outputPixels - 2, outputPixels - 2, color);
      }
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
    current += glyphWidth(codepoint, size);
  }
  return current > widest ? current : widest;
}

bool ChineseRenderer::loadFixed(uint32_t codepoint, uint8_t bitmap[32]) const {
  size_t low = 0;
  size_t high = kFixedChineseGlyphCount;
  while (low < high) {
    const size_t middle = low + (high - low) / 2;
    if (kFixedChineseGlyphs[middle].codepoint < codepoint) low = middle + 1;
    else high = middle;
  }
  if (low >= kFixedChineseGlyphCount ||
      kFixedChineseGlyphs[low].codepoint != codepoint) return false;
  memcpy(bitmap, kFixedChineseGlyphs[low].bitmap, 32);
  return true;
}

bool ChineseRenderer::loadSd(uint32_t codepoint, uint8_t bitmap[32]) {
  if (!sdFontReady_ || !fontFile_) return false;
  uint32_t low = 0;
  uint32_t high = sdGlyphCount_;
  uint8_t encoded[4];
  while (low < high) {
    const uint32_t middle = low + (high - low) / 2;
    if (!fontFile_.seek(16 + middle * sdEntrySize_) ||
        fontFile_.read(encoded, sizeof(encoded)) != sizeof(encoded)) return false;
    if (little32(encoded) < codepoint) low = middle + 1;
    else high = middle;
  }
  if (low >= sdGlyphCount_ || !fontFile_.seek(16 + low * sdEntrySize_) ||
      fontFile_.read(encoded, sizeof(encoded)) != sizeof(encoded) ||
      little32(encoded) != codepoint || fontFile_.read(bitmap, 32) != 32) return false;
  return true;
}

void ChineseRenderer::drawGlyph(int16_t x, int16_t y,
                                const uint8_t bitmap[32], uint16_t color,
                                uint16_t background, uint8_t pixelSize,
                                bool bold) {
  display_->fillRect(x, y, pixelSize, pixelSize, background);
  for (int16_t row = 0; row < 16; ++row) {
    const uint16_t bits = static_cast<uint16_t>(bitmap[row * 2]) << 8 |
                          bitmap[row * 2 + 1];
    const int16_t top = row * pixelSize / 16;
    const int16_t bottom = (row + 1) * pixelSize / 16;
    for (int16_t column = 0; column < 16; ++column) {
      if ((bits & (1U << (15 - column))) == 0) continue;
      const int16_t left = column * pixelSize / 16;
      const int16_t right = (column + 1) * pixelSize / 16;
      const int16_t width = right - left +
          ((bold && right < pixelSize) ? 1 : 0);
      display_->fillRect(x + left, y + top, width, bottom - top, color);
    }
  }
}

}  // namespace pokepod
