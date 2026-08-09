#include "UiIcons.h"

namespace pokepod {

void drawCapsuleMark(Arduino_GFX &display, int16_t centerX, int16_t centerY,
                     int16_t width, int16_t height, uint16_t color,
                     uint16_t fillColor) {
  const int16_t x = centerX - width / 2;
  const int16_t y = centerY - height / 2;
  const int16_t radius = height / 2;
  display.fillRoundRect(x, y, width, height, radius, fillColor);
  display.drawRoundRect(x, y, width, height, radius, color);
  display.drawRoundRect(x + 1, y + 1, width - 2, height - 2,
                        radius > 1 ? radius - 1 : radius, color);
  display.drawFastVLine(centerX, y + 3, height - 6, color);
}

void drawToggle(Arduino_GFX &display, int16_t x, int16_t y, bool enabled,
                uint16_t enabledColor, uint16_t disabledColor,
                uint16_t background) {
  const uint16_t track = enabled ? enabledColor : disabledColor;
  display.fillRoundRect(x, y, 42, 24, 12, track);
  display.fillCircle(enabled ? x + 30 : x + 12, y + 12, 8, background);
}

void drawUiIcon(Arduino_GFX &display, UiIcon icon, int16_t x, int16_t y,
                uint16_t color) {
  switch (icon) {
    case UiIcon::capsule:
      drawCapsuleMark(display, x + 12, y + 12, 22, 12, color, 0);
      break;
    case UiIcon::list:
      for (int16_t row = 0; row < 3; ++row) {
        display.fillCircle(x + 3, y + 5 + row * 7, 1, color);
        display.drawFastHLine(x + 8, y + 5 + row * 7, 14, color);
      }
      break;
    case UiIcon::device:
      display.drawFastHLine(x + 2, y + 5, 20, color);
      display.drawFastHLine(x + 2, y + 12, 20, color);
      display.drawFastHLine(x + 2, y + 19, 20, color);
      display.fillCircle(x + 8, y + 5, 3, color);
      display.fillCircle(x + 16, y + 12, 3, color);
      display.fillCircle(x + 10, y + 19, 3, color);
      break;
    case UiIcon::wifi:
      display.drawLine(x + 3, y + 9, x + 12, y + 3, color);
      display.drawLine(x + 12, y + 3, x + 21, y + 9, color);
      display.drawLine(x + 7, y + 14, x + 12, y + 10, color);
      display.drawLine(x + 12, y + 10, x + 17, y + 14, color);
      display.fillCircle(x + 12, y + 20, 2, color);
      break;
    case UiIcon::bluetooth:
      display.drawFastVLine(x + 12, y + 1, 22, color);
      display.drawLine(x + 12, y + 1, x + 20, y + 8, color);
      display.drawLine(x + 20, y + 8, x + 7, y + 18, color);
      display.drawLine(x + 12, y + 23, x + 20, y + 16, color);
      display.drawLine(x + 20, y + 16, x + 7, y + 6, color);
      break;
    case UiIcon::mac:
      display.drawRoundRect(x + 3, y + 4, 18, 14, 2, color);
      display.drawFastHLine(x + 7, y + 21, 10, color);
      display.drawFastVLine(x + 12, y + 18, 4, color);
      break;
    case UiIcon::storage:
      display.drawRoundRect(x + 4, y + 2, 16, 20, 2, color);
      display.drawFastHLine(x + 7, y + 17, 10, color);
      display.fillCircle(x + 16, y + 6, 1, color);
      break;
    case UiIcon::raise:
      display.drawLine(x + 12, y + 2, x + 12, y + 17, color);
      display.drawLine(x + 6, y + 8, x + 12, y + 2, color);
      display.drawLine(x + 18, y + 8, x + 12, y + 2, color);
      display.drawRoundRect(x + 4, y + 16, 16, 7, 3, color);
      break;
    case UiIcon::phone:
      display.drawRoundRect(x + 6, y + 1, 12, 22, 2, color);
      display.fillCircle(x + 12, y + 19, 1, color);
      break;
    case UiIcon::play:
      display.fillTriangle(x + 7, y + 4, x + 7, y + 20,
                           x + 20, y + 12, color);
      break;
    case UiIcon::stop:
      display.fillRoundRect(x + 6, y + 6, 12, 12, 2, color);
      break;
    case UiIcon::star:
      display.drawLine(x + 12, y + 2, x + 15, y + 9, color);
      display.drawLine(x + 15, y + 9, x + 22, y + 9, color);
      display.drawLine(x + 22, y + 9, x + 17, y + 14, color);
      display.drawLine(x + 17, y + 14, x + 19, y + 22, color);
      display.drawLine(x + 19, y + 22, x + 12, y + 17, color);
      display.drawLine(x + 12, y + 17, x + 5, y + 22, color);
      display.drawLine(x + 5, y + 22, x + 7, y + 14, color);
      display.drawLine(x + 7, y + 14, x + 2, y + 9, color);
      display.drawLine(x + 2, y + 9, x + 9, y + 9, color);
      display.drawLine(x + 9, y + 9, x + 12, y + 2, color);
      break;
    case UiIcon::archive:
      display.drawRect(x + 3, y + 7, 18, 14, color);
      display.drawRect(x + 2, y + 3, 20, 5, color);
      display.drawFastHLine(x + 9, y + 12, 6, color);
      break;
    case UiIcon::retry:
      display.drawCircle(x + 12, y + 12, 9, color);
      display.fillTriangle(x + 17, y + 2, x + 22, y + 4,
                           x + 18, y + 8, color);
      display.drawFastVLine(x + 12, y + 10, 7, color);
      display.fillCircle(x + 12, y + 20, 1, color);
      break;
    case UiIcon::back:
      display.drawLine(x + 17, y + 3, x + 7, y + 12, color);
      display.drawLine(x + 7, y + 12, x + 17, y + 21, color);
      break;
    case UiIcon::chevron:
      display.drawLine(x + 7, y + 3, x + 17, y + 12, color);
      display.drawLine(x + 17, y + 12, x + 7, y + 21, color);
      break;
    case UiIcon::warning:
      display.drawTriangle(x + 12, y + 2, x + 2, y + 21,
                           x + 22, y + 21, color);
      display.drawFastVLine(x + 12, y + 8, 7, color);
      display.fillCircle(x + 12, y + 18, 1, color);
      break;
    case UiIcon::check:
      display.drawLine(x + 3, y + 13, x + 9, y + 19, color);
      display.drawLine(x + 9, y + 19, x + 21, y + 5, color);
      break;
  }
}

}  // namespace pokepod
