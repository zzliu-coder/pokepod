#include <cassert>
#include <cmath>
#include <cstdint>

#include "../PokePodAmoled/UiTheme.h"

namespace {

double channel(uint8_t value) {
  const double normalized = value / 255.0;
  return normalized <= 0.04045
      ? normalized / 12.92
      : std::pow((normalized + 0.055) / 1.055, 2.4);
}

double luminance(uint32_t rgb) {
  return 0.2126 * channel(static_cast<uint8_t>(rgb >> 16)) +
      0.7152 * channel(static_cast<uint8_t>(rgb >> 8)) +
      0.0722 * channel(static_cast<uint8_t>(rgb));
}

double contrast(uint32_t left, uint32_t right) {
  const double a = luminance(left);
  const double b = luminance(right);
  const double lighter = a > b ? a : b;
  const double darker = a > b ? b : a;
  return (lighter + 0.05) / (darker + 0.05);
}

}  // namespace

int main() {
  using namespace pokepod::ui;
  assert(contrast(kInkRgb, kBackgroundRgb) >= 4.5);
  assert(contrast(kMutedRgb, kBackgroundRgb) >= 4.5);
  assert(contrast(kAccentRgb, kBackgroundRgb) >= 4.5);
  assert(contrast(kWirelessRgb, kBackgroundRgb) >= 4.5);
  assert(contrast(kWaitingRgb, kBackgroundRgb) >= 4.5);
  assert(contrast(kErrorRgb, kBackgroundRgb) >= 4.5);

  assert(kRootContentBottom < kPageIndicatorTop);
  assert(kPageIndicatorTop < kScreenHeight);
  assert(kHomePrimaryTop < kHomePrimaryConnectedBottom);
  assert(kHomePrimaryConnectedBottom < kHomeSecondaryTop);
  assert(kHomeSecondaryBottom <= kRootContentBottom);
  assert(kHomePrimarySoloBottom <= kRootContentBottom);
  assert(kCapsuleListTop >= kTopBarHeight + 48);
  assert(kCapsuleListBottom <= kRootContentBottom);
  assert(kDeviceRowsBottom <= kRootContentBottom);
  assert(kSettingIconLeft + kActionIconSize <= kSettingTextLeft);
  assert(kSettingTextLeft + kSettingTitleWidth <= kSettingTrailingLeft);
  assert(kSettingTrailingLeft + kSettingToggleWidth <= kSettingValueRight);
  assert(kSettingIconTopOffset + kActionIconSize / 2 == 32);
  assert(kSettingSingleTitleTopOffset + 20 / 2 == 32);
  assert(kSettingValueTopOffset + 16 / 2 == 32);
  assert(kSettingTwoLineTitleTopOffset + 20 <= kSettingDetailTopOffset);
  assert(kDetailActionsBottom <= kRootContentBottom);
  assert(kMinimumTouchHeight >= 56);
  assert(kTouchTapSlop >= 8 && kTouchTapSlop <= 16);
  assert(kTouchSwipeThreshold == 32);
  assert(kTouchVerticalThreshold == 12);
  assert(kWirelessHoldDelayMs == 280);
  assert(kBackTargetSize >= kMinimumTouchHeight);
  assert(kBackEdgeWidth == 40);
  assert(kStatusIconSize == 20);
  assert(kActionIconSize == 24);
  assert(kRecordingFrameIntervalMs >= 80);
  assert(kRecordingFrameIntervalMs <= 84);
  return 0;
}
