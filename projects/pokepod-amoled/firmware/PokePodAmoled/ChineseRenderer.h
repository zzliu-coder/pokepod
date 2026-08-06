#pragma once

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <FS.h>

namespace pokepod {

class ChineseRenderer {
 public:
  void begin(Arduino_GFX *display, fs::FS *fs = nullptr);
  bool sdFontReady() const { return sdFontReady_; }
  void drawText(const String &text, int16_t x, int16_t y, int16_t maxWidth,
                uint8_t maxLines, uint16_t color, uint16_t background,
                uint16_t skipLines = 0, bool preferSdFont = false);

 private:
  bool loadFixed(uint32_t codepoint, uint8_t bitmap[32]) const;
  bool loadSd(uint32_t codepoint, uint8_t bitmap[32]);
  void drawGlyph(int16_t x, int16_t y, const uint8_t bitmap[32],
                 uint16_t color, uint16_t background);

  Arduino_GFX *display_ = nullptr;
  File fontFile_;
  uint32_t sdGlyphCount_ = 0;
  uint32_t sdEntrySize_ = 0;
  bool sdFontReady_ = false;
};

}  // namespace pokepod
