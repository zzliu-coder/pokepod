#pragma once

#include <stdint.h>

#include "FirmwarePolicy.h"
#include "UiTheme.h"

namespace pokepod {

// Page values encode their physical order. Home is deliberately the center.
enum class RootPage : uint8_t { capsules = 0, home = 1, device = 2 };
enum class HomeMode : uint8_t { idle, recording, committing, queued, transcribing, success, failed };
enum class UiScreen : uint8_t { capsules, home, device, capsuleDetail, provisioning };

struct UiState {
  RootPage page = RootPage::home;
  HomeMode homeMode = HomeMode::idle;
  bool capsuleDetail = false;
  bool provisioning = false;
  bool detailRetryEnabled = false;
  int capsuleSelection = -1;

  UiScreen screen() const {
    if (provisioning) return UiScreen::provisioning;
    if (capsuleDetail) return UiScreen::capsuleDetail;
    if (page == RootPage::capsules) return UiScreen::capsules;
    if (page == RootPage::device) return UiScreen::device;
    return UiScreen::home;
  }
};

struct DeviceHealthState {
  bool ioExpander = false;
  bool display = false;
  bool touch = false;
  bool sdCard = false;
  bool rtc = false;
  bool imu = false;
  bool pmu = false;
  bool audio = false;
  bool usb = false;
  bool fullTextFont = false;

  bool ready() const {
    return ioExpander && display && touch && sdCard && rtc && imu && pmu &&
        audio && usb && fullTextFont;
  }
};

inline RootPage swipedPage(RootPage page, int16_t deltaX, bool locked) {
  if (locked || (deltaX > -60 && deltaX < 60)) return page;
  int value = static_cast<int>(page);
  value += deltaX < 0 ? 1 : -1;
  if (value < static_cast<int>(RootPage::capsules)) {
    value = static_cast<int>(RootPage::capsules);
  }
  if (value > static_cast<int>(RootPage::device)) {
    value = static_cast<int>(RootPage::device);
  }
  return static_cast<RootPage>(value);
}

inline bool isBackEdgeSwipe(int16_t startX, int16_t deltaX) {
  return startX >= 0 && startX < ui::kBackEdgeWidth && deltaX > 60;
}

inline uint16_t scrolledOffset(uint16_t current, int16_t deltaY,
                               bool canIncrease, uint16_t step) {
  if (deltaY < -40 && canIncrease) return current + step;
  if (deltaY > 40 && current >= step) return current - step;
  return current;
}

enum class UiAction : uint8_t {
  none,
  capsuleRecord,
  wechatDictation,
  openProvisioning,
  wifiToggle,
  raiseToWakeToggle,
  openCapsule,
  back,
  play,
  favorite,
  archive,
  retry,
};

inline UiAction uiActionAt(const UiState &state, int16_t x, int16_t y,
                           bool macConnected) {
  if (x < 0 || x >= kDisplayWidth || y < 0 || y >= kDisplayHeight) {
    return UiAction::none;
  }
  const UiScreen screen = state.screen();
  if (screen == UiScreen::provisioning) {
    return x < ui::kBackTargetSize && y < ui::kBackTargetSize
        ? UiAction::back : UiAction::none;
  }
  if (screen == UiScreen::capsuleDetail) {
    if (x < ui::kBackTargetSize && y < ui::kBackTargetSize) {
      return UiAction::back;
    }
    if (y >= ui::kDetailActionsTop && y < ui::kDetailActionsBottom) {
      if (x < 92) return UiAction::play;
      if (x < 184) return UiAction::favorite;
      if (x < 276) return UiAction::archive;
      return state.detailRetryEnabled ? UiAction::retry : UiAction::none;
    }
    return UiAction::none;
  }
  if (screen == UiScreen::home) {
    if (state.homeMode == HomeMode::recording) {
      return y >= ui::kRootContentTop && y < ui::kRootContentBottom
          ? UiAction::capsuleRecord : UiAction::none;
    }
    if (state.homeMode != HomeMode::idle) return UiAction::none;
    const int16_t recordBottom = macConnected
        ? ui::kHomePrimaryConnectedBottom : ui::kHomePrimarySoloBottom;
    if (y >= ui::kHomePrimaryTop && y < recordBottom) {
      return UiAction::capsuleRecord;
    }
    if (macConnected && y >= ui::kHomeSecondaryTop &&
        y < ui::kHomeSecondaryBottom) return UiAction::wechatDictation;
    return UiAction::none;
  }
  if (screen == UiScreen::device) {
    if (y >= ui::kDeviceWifiTop && y < ui::kDeviceMacTop) {
      return UiAction::wifiToggle;
    }
    if (y >= ui::kDeviceRaiseTop && y < ui::kDeviceProvisionTop) {
      return UiAction::raiseToWakeToggle;
    }
    if (y >= ui::kDeviceProvisionTop && y < ui::kDeviceRowsBottom) {
      return UiAction::openProvisioning;
    }
    return UiAction::none;
  }
  if (screen == UiScreen::capsules && y >= ui::kCapsuleListTop &&
      y < ui::kCapsuleListBottom) return UiAction::openCapsule;
  return UiAction::none;
}

}  // namespace pokepod
