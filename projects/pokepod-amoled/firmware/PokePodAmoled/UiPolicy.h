#pragma once

#include <stdint.h>

#include "FirmwarePolicy.h"
#include "UiTheme.h"

namespace pokepod {

enum class RootPage : uint8_t { home = 0, capsules = 1, device = 2 };
enum class HomeMode : uint8_t { idle, recording, committing, queued, transcribing, success, failed };

struct UiState {
  RootPage page = RootPage::home;
  HomeMode homeMode = HomeMode::idle;
  bool capsuleDetail = false;
  bool detailRetryEnabled = false;
  int capsuleSelection = -1;
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
  if (value < 0) value = 0;
  if (value > 2) value = 2;
  return static_cast<RootPage>(value);
}

enum class UiAction : uint8_t {
  none,
  goHome,
  goCapsules,
  goDevice,
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
  if (x < 0 || x >= kDisplayWidth || y < 0 || y >= kDisplayHeight) return UiAction::none;
  if (state.capsuleDetail) {
    if (y < 64) return UiAction::back;
    if (y >= ui::kDetailActionsTop && y < ui::kDetailActionsBottom) {
      if (x < 92) return UiAction::play;
      if (x < 184) return UiAction::favorite;
      if (x < 276) return UiAction::archive;
      return state.detailRetryEnabled ? UiAction::retry : UiAction::none;
    }
    return UiAction::none;
  }
  if (y >= ui::kBottomNavTop && state.homeMode != HomeMode::recording) {
    if (x < ui::kScreenWidth / 3) return UiAction::goHome;
    if (x < ui::kScreenWidth * 2 / 3) return UiAction::goCapsules;
    return UiAction::goDevice;
  }
  if (state.page == RootPage::home) {
    if (state.homeMode == HomeMode::recording) {
      return y >= 80 && y < ui::kBottomNavTop
          ? UiAction::capsuleRecord : UiAction::none;
    }
    if (state.homeMode != HomeMode::idle) return UiAction::none;
    if (y >= ui::kHomeRecordTop && y < ui::kHomeRecordBottom) {
      return UiAction::capsuleRecord;
    }
    if (macConnected && y >= ui::kDictationTop &&
        y < ui::kDictationBottom) return UiAction::wechatDictation;
  }
  if (state.page == RootPage::device) {
    if (y >= ui::kDeviceWifiTop && y < ui::kDeviceMacTop) {
      return UiAction::wifiToggle;
    }
    if (y >= ui::kDeviceRaiseTop && y < ui::kDeviceProvisionTop) {
      return UiAction::raiseToWakeToggle;
    }
    if (y >= ui::kDeviceProvisionTop && y < ui::kDeviceRowsBottom) {
      return UiAction::openProvisioning;
    }
  }
  if (state.page == RootPage::capsules && y >= ui::kCapsuleListTop &&
      y < ui::kCapsuleListBottom) return UiAction::openCapsule;
  return UiAction::none;
}

}  // namespace pokepod
