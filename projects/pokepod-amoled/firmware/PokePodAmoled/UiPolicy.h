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

inline int16_t touchMagnitude(int16_t value) {
  return value < 0 ? -value : value;
}

inline bool touchTapEligible(int16_t maximumDeltaX,
                             int16_t maximumDeltaY) {
  return touchMagnitude(maximumDeltaX) <= ui::kTouchTapSlop &&
      touchMagnitude(maximumDeltaY) <= ui::kTouchTapSlop;
}

inline bool touchHorizontalSwipe(int16_t deltaX, int16_t deltaY) {
  return touchMagnitude(deltaX) >= ui::kTouchSwipeThreshold &&
      touchMagnitude(deltaX) > touchMagnitude(deltaY);
}

inline bool touchVerticalSwipe(int16_t deltaX, int16_t deltaY) {
  return touchMagnitude(deltaY) >= ui::kTouchVerticalThreshold &&
      touchMagnitude(deltaY) > touchMagnitude(deltaX);
}

inline bool dictationHoldReady(uint32_t elapsedMs, int16_t maximumDeltaX,
                               int16_t maximumDeltaY) {
  return elapsedMs >= ui::kDictationHoldDelayMs &&
      touchTapEligible(maximumDeltaX, maximumDeltaY);
}

struct TouchGestureTracker {
  bool active = false;
  int16_t startX = 0;
  int16_t startY = 0;
  int16_t lastX = 0;
  int16_t lastY = 0;
  int16_t maximumDeltaX = 0;
  int16_t maximumDeltaY = 0;
  uint32_t startedAtMs = 0;

  void begin(int16_t x, int16_t y, uint32_t nowMs) {
    active = true;
    startX = lastX = x;
    startY = lastY = y;
    maximumDeltaX = 0;
    maximumDeltaY = 0;
    startedAtMs = nowMs;
  }

  void update(int16_t x, int16_t y) {
    if (!active) return;
    lastX = x;
    lastY = y;
    const int16_t currentDeltaX = deltaX();
    const int16_t currentDeltaY = deltaY();
    if (touchMagnitude(currentDeltaX) > touchMagnitude(maximumDeltaX)) {
      maximumDeltaX = currentDeltaX;
    }
    if (touchMagnitude(currentDeltaY) > touchMagnitude(maximumDeltaY)) {
      maximumDeltaY = currentDeltaY;
    }
  }

  int16_t deltaX() const { return lastX - startX; }
  int16_t deltaY() const { return lastY - startY; }
  bool tapEligible() const {
    return touchTapEligible(maximumDeltaX, maximumDeltaY);
  }
  bool horizontalSwipe() const {
    return touchHorizontalSwipe(deltaX(), deltaY());
  }
  bool verticalSwipe() const {
    return touchVerticalSwipe(deltaX(), deltaY());
  }
  bool dictationReady(uint32_t nowMs) const {
    return active && dictationHoldReady(nowMs - startedAtMs,
                                        maximumDeltaX, maximumDeltaY);
  }
  void reset() { active = false; }
};

inline RootPage swipedPage(RootPage page, int16_t deltaX, bool locked) {
  if (locked || touchMagnitude(deltaX) < ui::kTouchSwipeThreshold) return page;
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
  return startX >= 0 && startX < ui::kBackEdgeWidth &&
      deltaX > ui::kTouchSwipeThreshold;
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
    const bool backButton = x < ui::kBackTargetSize &&
        y < ui::kBackTargetSize;
    const bool exitButton = y >= ui::kProvisionExitTop &&
        y < ui::kProvisionExitBottom;
    return backButton || exitButton ? UiAction::back : UiAction::none;
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
