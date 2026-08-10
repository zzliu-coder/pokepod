#pragma once

#include <Arduino.h>
#include "PokePodGfx.h"
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
                UiTextSize size = UiTextSize::compact, bool bold = false,
                int16_t pixelOffset = 0, int16_t clipTop = -32768,
                int16_t clipBottom = 32767);
  int16_t measureTextWidth(const String &text,
                           UiTextSize size = UiTextSize::compact) const;
  int16_t textLineHeight(UiTextSize size) const;
  uint16_t wrappedLineCount(const String &text, int16_t maxWidth,
                            UiTextSize size = UiTextSize::body) const;

 private:
  static constexpr uint16_t kMaximumGlyphBytes = (36 * 36 + 1) / 2;
  struct GlyphData {
    uint8_t pixels = 0;
    uint8_t advance = 0;
    uint8_t bitmap[kMaximumGlyphBytes] = {};
  };
  static constexpr uint16_t kSdGlyphBitmapBytes = (20 * 20 + 1) / 2;
  static constexpr uint16_t kSdGlyphCacheEntries = 128;
  struct SdGlyphCacheEntry {
    uint32_t codepoint = 0;
    uint32_t age = 0;
    uint8_t advance = 0;
    bool valid = false;
    uint8_t bitmap[kSdGlyphBitmapBytes] = {};
  };

  bool loadFixed(uint32_t codepoint, UiTextSize size, GlyphData &glyph) const;
  bool loadSd(uint32_t codepoint, UiTextSize size, GlyphData &glyph);
  bool loadSdCache(uint32_t codepoint, GlyphData &glyph);
  void storeSdCache(uint32_t codepoint, const GlyphData &glyph);
  uint8_t fixedAdvance(uint32_t codepoint, UiTextSize size) const;
  void drawGlyph(int16_t x, int16_t y, const GlyphData &glyph,
                 uint16_t color, uint16_t background, bool bold,
                 int16_t clipTop, int16_t clipBottom);

  Arduino_GFX *display_ = nullptr;
  File fontFile_;
  uint32_t sdGlyphCount_ = 0;
  uint32_t sdEntrySize_ = 0;
  uint32_t sdCacheAge_ = 0;
  SdGlyphCacheEntry *sdCache_ = nullptr;
  bool sdFontReady_ = false;
};

}  // namespace pokepod
