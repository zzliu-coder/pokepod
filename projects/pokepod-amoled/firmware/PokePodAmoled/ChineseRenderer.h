#pragma once

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <FS.h>

namespace pokepod {

enum class UiTextSize : uint8_t {
  compact,  // 16 px status and metadata
  body,     // 20 px body and controls
  display,  // 28 px page and action titles
  timer,    // 36 px recording timer only
};

class ChineseRenderer {
 public:
  void begin(Arduino_GFX *display, fs::FS *fs = nullptr);
  bool sdFontReady() const { return sdFontReady_; }
  void drawText(const String &text, int16_t x, int16_t y, int16_t maxWidth,
                uint8_t maxLines, uint16_t color, uint16_t background,
                uint16_t skipLines = 0, bool preferSdFont = false,
                UiTextSize size = UiTextSize::compact, bool bold = false);
  int16_t measureTextWidth(const String &text,
                           UiTextSize size = UiTextSize::compact) const;

 private:
  static constexpr uint16_t kMaximumGlyphBytes = (36 * 36 + 1) / 2;
  struct GlyphData {
    uint8_t pixels = 0;
    uint8_t advance = 0;
    uint8_t bitmap[kMaximumGlyphBytes] = {};
  };

  bool loadFixed(uint32_t codepoint, UiTextSize size, GlyphData &glyph) const;
  bool loadSd(uint32_t codepoint, UiTextSize size, GlyphData &glyph);
  uint8_t fixedAdvance(uint32_t codepoint, UiTextSize size) const;
  void drawGlyph(int16_t x, int16_t y, const GlyphData &glyph,
                 uint16_t color, uint16_t background, bool bold);

  Arduino_GFX *display_ = nullptr;
  File fontFile_;
  uint32_t sdGlyphCount_ = 0;
  uint32_t sdEntrySize_ = 0;
  bool sdFontReady_ = false;
};

}  // namespace pokepod
