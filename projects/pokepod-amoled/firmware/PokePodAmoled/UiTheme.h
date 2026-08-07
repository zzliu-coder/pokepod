#pragma once

#include <stdint.h>

namespace pokepod {
namespace ui {

constexpr uint16_t color565(uint32_t rgb) {
  return static_cast<uint16_t>(
      ((rgb >> 8) & 0xF800U) |
      ((rgb >> 5) & 0x07E0U) |
      ((rgb >> 3) & 0x001FU));
}

constexpr uint32_t kBackgroundRgb = 0x000000;
constexpr uint32_t kSurfaceRgb = 0x0B1311;
constexpr uint32_t kSurfaceRaisedRgb = 0x101A17;
constexpr uint32_t kDividerRgb = 0x1A2A25;
constexpr uint32_t kInkRgb = 0xF4FAF7;
constexpr uint32_t kMutedRgb = 0x8EA39C;
constexpr uint32_t kDisabledRgb = 0x34413C;
constexpr uint32_t kAccentRgb = 0x69E0B6;
constexpr uint32_t kAccentDimRgb = 0x12372C;
constexpr uint32_t kDictationRgb = 0xA0B2FF;
constexpr uint32_t kDictationDimRgb = 0x151A2C;
constexpr uint32_t kWaitingRgb = 0xF0C45B;
constexpr uint32_t kErrorRgb = 0xFF786D;

constexpr uint16_t kBackground = color565(kBackgroundRgb);
constexpr uint16_t kSurface = color565(kSurfaceRgb);
constexpr uint16_t kSurfaceRaised = color565(kSurfaceRaisedRgb);
constexpr uint16_t kDivider = color565(kDividerRgb);
constexpr uint16_t kInk = color565(kInkRgb);
constexpr uint16_t kMuted = color565(kMutedRgb);
constexpr uint16_t kDisabled = color565(kDisabledRgb);
constexpr uint16_t kAccent = color565(kAccentRgb);
constexpr uint16_t kAccentDim = color565(kAccentDimRgb);
constexpr uint16_t kDictation = color565(kDictationRgb);
constexpr uint16_t kDictationDim = color565(kDictationDimRgb);
constexpr uint16_t kWaiting = color565(kWaitingRgb);
constexpr uint16_t kError = color565(kErrorRgb);

constexpr int16_t kScreenWidth = 368;
constexpr int16_t kScreenHeight = 448;
constexpr int16_t kPageMargin = 24;
constexpr int16_t kTopBarHeight = 44;
constexpr int16_t kBottomNavTop = 400;
constexpr int16_t kBottomNavHeight = 48;

constexpr int16_t kHomeRecordTop = 108;
constexpr int16_t kHomeRecordBottom = 338;
constexpr int16_t kDictationTop = 344;
constexpr int16_t kDictationBottom = 394;

constexpr int16_t kCapsuleListTop = 120;
constexpr int16_t kCapsuleRowStride = 68;
constexpr int16_t kCapsuleVisibleRows = 4;
constexpr int16_t kCapsuleListBottom =
    kCapsuleListTop + kCapsuleRowStride * kCapsuleVisibleRows;

constexpr int16_t kDeviceWifiTop = 108;
constexpr int16_t kDeviceMacTop = 164;
constexpr int16_t kDeviceStorageTop = 220;
constexpr int16_t kDeviceRaiseTop = 276;
constexpr int16_t kDeviceProvisionTop = 332;
constexpr int16_t kDeviceRowsBottom = 392;

constexpr int16_t kDetailActionsTop = 332;
constexpr int16_t kDetailActionsBottom = 396;
constexpr uint32_t kRecordingFrameIntervalMs = 125;

}  // namespace ui
}  // namespace pokepod
