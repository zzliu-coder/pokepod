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
constexpr uint32_t kMutedRgb = 0x91A69F;
constexpr uint32_t kDisabledRgb = 0x34413C;
constexpr uint32_t kAccentRgb = 0x69E0B6;
constexpr uint32_t kAccentDimRgb = 0x12372C;
constexpr uint32_t kWirelessRgb = 0xA0B2FF;
constexpr uint32_t kWirelessDimRgb = 0x151A2C;
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
constexpr uint16_t kWireless = color565(kWirelessRgb);
constexpr uint16_t kWirelessDim = color565(kWirelessDimRgb);
constexpr uint16_t kWaiting = color565(kWaitingRgb);
constexpr uint16_t kError = color565(kErrorRgb);

constexpr int16_t kScreenWidth = 368;
constexpr int16_t kScreenHeight = 448;
constexpr int16_t kPageMargin = 20;
constexpr int16_t kTopBarHeight = 48;
constexpr int16_t kBackTargetSize = 56;
constexpr int16_t kBackEdgeWidth = 40;
constexpr int16_t kRootContentTop = 48;
constexpr int16_t kRootContentBottom = 424;
constexpr int16_t kPageIndicatorTop = 432;

constexpr int16_t kHomePrimaryTop = 72;
constexpr int16_t kHomePrimaryConnectedBottom = 238;
constexpr int16_t kHomePrimarySoloBottom = 416;
constexpr int16_t kHomeSecondaryTop = 250;
constexpr int16_t kHomeSecondaryBottom = 416;

constexpr int16_t kCapsuleListTop = 104;
constexpr int16_t kCapsuleRowStride = 76;
constexpr int16_t kCapsuleVisibleRows = 4;
constexpr int16_t kCapsuleListBottom =
    kCapsuleListTop + kCapsuleRowStride * kCapsuleVisibleRows;
constexpr int16_t kCapsuleSelectionBarTop = 352;
constexpr int16_t kCapsuleSelectionBarBottom = 424;
constexpr int16_t kScopePickerLeft = 24;
constexpr int16_t kScopePickerRight = 344;
constexpr int16_t kScopePickerTop = 82;
constexpr int16_t kScopePickerRowHeight = 48;
constexpr int16_t kScopePickerBottom =
    kScopePickerTop + kScopePickerRowHeight * 6;

constexpr int16_t kDeviceWifiTop = 88;
constexpr int16_t kDeviceMacTop = 152;
constexpr int16_t kDeviceStorageTop = 216;
constexpr int16_t kDeviceRaiseTop = 280;
constexpr int16_t kDeviceProvisionTop = 344;
constexpr int16_t kDeviceRowsBottom = 416;
constexpr int16_t kBluetoothPairTop = 156;
constexpr int16_t kBluetoothPairBottom = 240;
constexpr int16_t kBluetoothForgetTop = 260;
constexpr int16_t kBluetoothForgetBottom = 344;
constexpr int16_t kComputerSyncCloseTop = 356;
constexpr int16_t kComputerSyncCloseBottom = 424;
constexpr int16_t kSettingIconLeft = 20;
constexpr int16_t kSettingTextLeft = 58;
constexpr int16_t kSettingTrailingLeft = 302;
constexpr int16_t kSettingValueRight = 344;
constexpr int16_t kSettingIconTopOffset = 20;
constexpr int16_t kSettingSingleTitleTopOffset = 22;
constexpr int16_t kSettingTwoLineTitleTopOffset = 6;
constexpr int16_t kSettingDetailTopOffset = 38;
constexpr int16_t kSettingValueTopOffset = 24;
constexpr int16_t kSettingTitleWidth = 220;
constexpr int16_t kSettingTitleWithValueWidth = 178;
constexpr int16_t kSettingToggleWidth = 42;
constexpr int16_t kProvisionExitTop = 360;
constexpr int16_t kProvisionExitBottom = 424;
constexpr int16_t kProvisionLogSplit = 184;
constexpr int16_t kProvisionLogListTop = 88;
constexpr int16_t kProvisionLogRowStride = 94;
constexpr int16_t kProvisionLogVisibleRows = 3;
constexpr int16_t kProvisionLogListBottom = 424;

constexpr int16_t kDetailActionsTop = 352;
constexpr int16_t kDetailActionsBottom = 424;
constexpr int16_t kDetailTextTop = 88;
constexpr int16_t kDetailTextBottom = kDetailActionsTop;
constexpr int16_t kDetailMoreLeft = 34;
constexpr int16_t kDetailMoreRight = 334;
constexpr int16_t kDetailMoreTop = 202;
constexpr int16_t kDetailMoreRowHeight = 68;
constexpr int16_t kDetailMoreBottom =
    kDetailMoreTop + kDetailMoreRowHeight * 2;
constexpr int16_t kPurgeConfirmLeft = 28;
constexpr int16_t kPurgeConfirmRight = 340;
constexpr int16_t kPurgeConfirmTop = 132;
constexpr int16_t kPurgeConfirmBottom = 344;
constexpr int16_t kPurgeConfirmActionsTop = 274;
constexpr int16_t kPurgeConfirmActionSplit = 184;
constexpr int16_t kRecordingDynamicTop = 72;
constexpr int16_t kRecordingDynamicBottom = 326;
constexpr uint32_t kRecordingFrameIntervalMs = 80;
constexpr uint32_t kScrollFrameIntervalMs = 40;

constexpr int16_t kStatusIconSize = 20;
constexpr int16_t kActionIconSize = 24;
constexpr int16_t kMinimumTouchHeight = 56;
constexpr int16_t kTouchTapSlop = 12;
constexpr int16_t kTouchSwipeThreshold = 32;
constexpr int16_t kTouchVerticalThreshold = 12;
constexpr uint32_t kWirelessHoldDelayMs = 280;

}  // namespace ui
}  // namespace pokepod
