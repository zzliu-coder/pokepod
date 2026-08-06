#include "ChineseRenderer.h"

#include "CapsulePolicy.h"
#include "FixedChineseFont.h"
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
  if (!fontFile_ || fontFile_.read(header, sizeof(header)) != sizeof(header) ||
      memcmp(header, "PKF1", 4) != 0 || little16(header + 4) != 16 ||
      little16(header + 6) != 16 || little32(header + 12) != 36) {
    if (fontFile_) fontFile_.close();
    return;
  }
  sdGlyphCount_ = little32(header + 8);
  sdEntrySize_ = little32(header + 12);
  sdFontReady_ = sdGlyphCount_ > 0;
}

void ChineseRenderer::drawText(const String &text, int16_t x, int16_t y,
                               int16_t maxWidth, uint8_t maxLines,
                               uint16_t color, uint16_t background,
                               uint16_t skipLines, bool preferSdFont) {
  if (display_ == nullptr || maxLines == 0 || maxWidth < 12) return;
  size_t offset = 0;
  uint16_t logicalLine = 0;
  int16_t cursorX = 0;
  while (offset < text.length()) {
    const uint32_t codepoint = decodeUtf8(text.c_str(), text.length(), offset);
    const int16_t width = codepoint < 0x80 ? 12 : 16;
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
    const int16_t drawY = y + (logicalLine - skipLines) * 20;
    if (codepoint < 0x80) {
      display_->setTextSize(2);
      display_->setTextColor(color, background);
      display_->setCursor(x + cursorX, drawY);
      display_->write(static_cast<uint8_t>(codepoint));
    } else {
      uint8_t bitmap[32];
      const bool found = (preferSdFont && loadSd(codepoint, bitmap)) ||
                         loadFixed(codepoint, bitmap) ||
                         (!preferSdFont && loadSd(codepoint, bitmap));
      if (found) {
        drawGlyph(x + cursorX, drawY, bitmap, color, background);
      } else {
        display_->drawRect(x + cursorX + 1, drawY + 1, 14, 14, color);
      }
    }
    cursorX += width;
  }
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
                                uint16_t background) {
  display_->fillRect(x, y, 16, 16, background);
  for (int16_t row = 0; row < 16; ++row) {
    const uint16_t bits = static_cast<uint16_t>(bitmap[row * 2]) << 8 |
                          bitmap[row * 2 + 1];
    int16_t runStart = -1;
    for (int16_t column = 0; column <= 16; ++column) {
      const bool set = column < 16 && (bits & (1U << (15 - column))) != 0;
      if (set && runStart < 0) runStart = column;
      if (!set && runStart >= 0) {
        display_->drawFastHLine(x + runStart, y + row, column - runStart, color);
        runStart = -1;
      }
    }
  }
}

}  // namespace pokepod
