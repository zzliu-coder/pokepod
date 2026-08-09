#pragma once

#include <Arduino_GFX_Library.h>

namespace pokepod {

enum class UiIcon : uint8_t {
  capsule,
  list,
  device,
  wifi,
  bluetooth,
  mac,
  storage,
  raise,
  phone,
  play,
  stop,
  star,
  archive,
  retry,
  back,
  chevron,
  warning,
  check,
};

void drawUiIcon(Arduino_GFX &display, UiIcon icon, int16_t x, int16_t y,
                uint16_t color);
void drawCapsuleMark(Arduino_GFX &display, int16_t centerX, int16_t centerY,
                     int16_t width, int16_t height, uint16_t color,
                     uint16_t fillColor);
void drawToggle(Arduino_GFX &display, int16_t x, int16_t y, bool enabled,
                uint16_t enabledColor, uint16_t disabledColor,
                uint16_t background);

}  // namespace pokepod
