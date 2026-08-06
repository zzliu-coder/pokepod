#pragma once

#include <stdint.h>

#include "FirmwarePolicy.h"

namespace pokepod {

enum class RootPage : uint8_t { capsules = 0, home = 1, device = 2 };
enum class HomeMode : uint8_t { idle, recording, committing, queued, transcribing, success, failed };

struct UiState {
  RootPage page = RootPage::home;
  HomeMode homeMode = HomeMode::idle;
  bool capsuleDetail = false;
  int capsuleSelection = -1;
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
    if (y >= 356 && y < 436) {
      if (x < 92) return UiAction::play;
      if (x < 184) return UiAction::favorite;
      if (x < 276) return UiAction::archive;
      return UiAction::retry;
    }
    return UiAction::none;
  }
  if (state.page == RootPage::home) {
    if (state.homeMode == HomeMode::recording) {
      return y >= 300 && y < 420 ? UiAction::capsuleRecord : UiAction::none;
    }
    if (state.homeMode != HomeMode::idle) return UiAction::none;
    if (y >= 170 && y < 270) return UiAction::capsuleRecord;
    if (macConnected && y >= 290 && y < 390) return UiAction::wechatDictation;
  }
  if (state.page == RootPage::device) {
    if (y >= 170 && y < 240) return UiAction::wifiToggle;
    if (y >= 250 && y < 330) return UiAction::openProvisioning;
    if (y >= 334 && y < 406) return UiAction::raiseToWakeToggle;
  }
  if (state.page == RootPage::capsules && y >= 72 && y < 412) return UiAction::openCapsule;
  return UiAction::none;
}

}  // namespace pokepod
