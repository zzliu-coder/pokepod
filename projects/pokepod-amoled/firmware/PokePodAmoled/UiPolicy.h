#pragma once

#include <stdint.h>

#include "FirmwarePolicy.h"
#include "UiTheme.h"

namespace pokepod {

// A transient notice carries its visual severity explicitly.  The renderer
// must never infer severity from translated message text: copy changes and
// localization must not turn a progress message into an error.
enum class UiNoticeKind : uint8_t {
  info = 0,
  progress,
  success,
  warning,
  error,
};

inline bool uiNoticeUsesErrorIcon(UiNoticeKind kind) {
  return kind == UiNoticeKind::error;
}

inline bool uiNoticeUsesCheckIcon(UiNoticeKind kind) {
  return kind == UiNoticeKind::success;
}

inline bool uiNoticeIsProgress(UiNoticeKind kind) {
  return kind == UiNoticeKind::progress;
}

// BOOT is a physical two-function control.  A short press owns local
// recording; a held press owns wireless voice once the hold threshold is
// crossed.  This policy keeps the decision independent from the application
// services so it can be tested without Arduino or hardware mocks.
struct BootGestureContext {
  bool screenOn = true;
  bool provisioning = false;
  bool sensitiveConfirmationPending = false;
  bool wirelessHolding = false;
  bool localRecording = false;
  bool bluetoothEnabled = true;
  bool wirelessAppReady = false;
};

enum class BootGestureAction : uint8_t {
  none = 0,
  wakeScreen,
  confirmProvisioning,
  armProvisioningExit,
  exitProvisioning,
  stopWirelessVoice,
  startWirelessVoice,
  wirelessUnavailable,
  voiceReadyShortPress,
  bluetoothDisabled,
  stopLocalRecording,
  startLocalRecording,
};

class BootGesturePolicy {
 public:
  BootGestureAction pressed(uint32_t nowMs,
                            const BootGestureContext &context) {
    reset();
    active_ = true;
    startedAtMs_ = nowMs;
    localRecordingAtPress_ = context.localRecording;
    if (!context.screenOn) {
      wakeOnly_ = true;
      return BootGestureAction::wakeScreen;
    }
    if (context.provisioning) {
      provisioning_ = true;
      if (context.sensitiveConfirmationPending) {
        provisioningConfirmationConsumed_ = true;
        return BootGestureAction::confirmProvisioning;
      }
      provisioningExitArmed_ = true;
      return BootGestureAction::armProvisioningExit;
    }
    if (context.wirelessHolding) {
      wirelessHoldingAtPress_ = true;
    }
    return BootGestureAction::none;
  }

  BootGestureAction held(uint32_t nowMs,
                         const BootGestureContext &context) {
    if (!active_ || wakeOnly_ || provisioning_ ||
        wirelessHoldingAtPress_ || localRecordingAtPress_ ||
        context.wirelessHolding || context.localRecording ||
        longActionConsumed_ || nowMs - startedAtMs_ < ui::kWirelessHoldDelayMs) {
      return BootGestureAction::none;
    }
    longActionConsumed_ = true;
    if (!context.bluetoothEnabled) return BootGestureAction::bluetoothDisabled;
    if (!context.wirelessAppReady) {
      return BootGestureAction::wirelessUnavailable;
    }
    return BootGestureAction::startWirelessVoice;
  }

  BootGestureAction released(const BootGestureContext &context) {
    if (!active_) return BootGestureAction::none;
    active_ = false;
    if (wakeOnly_) return BootGestureAction::none;
    if (provisioningConfirmationConsumed_) {
      provisioningConfirmationConsumed_ = false;
      return BootGestureAction::none;
    }
    if (provisioningExitArmed_) {
      provisioningExitArmed_ = false;
      return BootGestureAction::exitProvisioning;
    }
    if (wirelessHoldingAtPress_ || context.wirelessHolding) {
      return BootGestureAction::stopWirelessVoice;
    }
    // A long gesture that could not start wireless input is consumed.  This
    // is the critical guard that prevents a failed long press from starting a
    // local capsule on release.
    if (longActionConsumed_) return BootGestureAction::none;
    // When the Mac voice path is ready, a short BOOT tap belongs to the
    // on-screen capsule control.  Keep the physical button's early release
    // from starting a second capture owner.
    if (context.wirelessAppReady) {
      return BootGestureAction::voiceReadyShortPress;
    }
    return localRecordingAtPress_ ? BootGestureAction::stopLocalRecording
                                  : BootGestureAction::startLocalRecording;
  }

  bool active() const { return active_; }
  bool longActionConsumed() const { return longActionConsumed_; }

  void reset() {
    active_ = false;
    wakeOnly_ = false;
    provisioning_ = false;
    provisioningExitArmed_ = false;
    provisioningConfirmationConsumed_ = false;
    wirelessHoldingAtPress_ = false;
    localRecordingAtPress_ = false;
    longActionConsumed_ = false;
    startedAtMs_ = 0;
  }

 private:
  bool active_ = false;
  bool wakeOnly_ = false;
  bool provisioning_ = false;
  bool provisioningExitArmed_ = false;
  bool provisioningConfirmationConsumed_ = false;
  bool wirelessHoldingAtPress_ = false;
  bool localRecordingAtPress_ = false;
  bool longActionConsumed_ = false;
  uint32_t startedAtMs_ = 0;
};

// Page values encode their physical order. Home is deliberately the center.
enum class RootPage : uint8_t { capsules = 0, home = 1, device = 2 };
enum class HomeMode : uint8_t { idle, recording, committing, queued, transcribing, success, failed };
enum class UiScreen : uint8_t {
  capsules,
  home,
  device,
  computerSync,
  bluetoothPairing,
  capsuleDetail,
  provisioning,
  provisioningLog,
};

struct UiState {
  RootPage page = RootPage::home;
  HomeMode homeMode = HomeMode::idle;
  bool capsuleDetail = false;
  bool computerSync = false;
  bool bluetoothPairing = false;
  bool provisioning = false;
  bool provisioningLog = false;
  bool detailRetryEnabled = false;
  bool detailTrashEnabled = false;
  bool detailMoreOverlay = false;
  bool purgeConfirmOverlay = false;
  bool shutdownConfirmOverlay = false;
  bool capsuleScopeOverlay = false;
  bool capsuleSelectionMode = false;
  bool capsuleTrashScope = false;
  bool undoAvailable = false;

  UiScreen screen() const {
    if (provisioning && provisioningLog) return UiScreen::provisioningLog;
    if (provisioning) return UiScreen::provisioning;
    if (computerSync) return UiScreen::computerSync;
    if (bluetoothPairing) return UiScreen::bluetoothPairing;
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
  bool capsuleLibrary = false;
  bool recorder = false;
  bool transcription = false;
  bool bleVoice = false;
  bool link = false;
  bool wifi = false;
  bool fullTextFont = false;

  bool ready() const {
    return ioExpander && display && touch && sdCard && rtc && imu && pmu &&
        audio && usb && capsuleLibrary && recorder && transcription &&
        bleVoice && link && wifi && fullTextFont;
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
  const int32_t horizontal = touchMagnitude(deltaX);
  const int32_t vertical = touchMagnitude(deltaY);
  return horizontal >= ui::kTouchSwipeThreshold &&
      horizontal * 100 > vertical * 135;
}

inline bool touchVerticalSwipe(int16_t deltaX, int16_t deltaY) {
  return touchMagnitude(deltaY) >= ui::kTouchVerticalThreshold &&
      touchMagnitude(deltaY) > touchMagnitude(deltaX);
}

inline bool wirelessHoldReady(uint32_t elapsedMs, int16_t maximumDeltaX,
                              int16_t maximumDeltaY) {
  return elapsedMs >= ui::kWirelessHoldDelayMs &&
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
  bool wirelessHoldReady(uint32_t nowMs) const {
    return active && pokepod::wirelessHoldReady(nowMs - startedAtMs,
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

enum class UiAction : uint8_t {
  none,
  capsuleRecord,
  wechatVoice,
  openProvisioning,
  openProvisioningLog,
  wifiToggle,
  bluetoothToggle,
  openBluetoothPairing,
  toggleBluetoothPairing,
  forgetBluetoothMac,
  openComputerSync,
  openShutdownConfirm,
  confirmShutdown,
  closeComputerSync,
  raiseToWakeToggle,
  openCapsule,
  openCapsuleScope,
  selectScopeInbox,
  selectScopeFavorites,
  selectScopePending,
  selectScopeFailed,
  selectScopeArchive,
  selectScopeTrash,
  openDetailMore,
  closeOverlay,
  back,
  play,
  favorite,
  archive,
  retry,
  trash,
  requestPurge,
  confirmPurge,
  bulkFavorite,
  bulkArchive,
  bulkTrash,
  undoTrash,
};

inline bool uiActionRequiresCapsuleLibrary(UiAction action) {
  switch (action) {
    case UiAction::undoTrash:
    case UiAction::openCapsule:
    case UiAction::openCapsuleScope:
    case UiAction::selectScopeInbox:
    case UiAction::selectScopeFavorites:
    case UiAction::selectScopePending:
    case UiAction::selectScopeFailed:
    case UiAction::selectScopeArchive:
    case UiAction::selectScopeTrash:
    case UiAction::openDetailMore:
    case UiAction::play:
    case UiAction::favorite:
    case UiAction::archive:
    case UiAction::retry:
    case UiAction::trash:
    case UiAction::requestPurge:
    case UiAction::confirmPurge:
    case UiAction::bulkFavorite:
    case UiAction::bulkArchive:
    case UiAction::bulkTrash:
      return true;
    default:
      return false;
  }
}

inline int8_t capsuleScopeIndexForAction(UiAction action) {
  switch (action) {
    case UiAction::selectScopeInbox: return 0;
    case UiAction::selectScopeFavorites: return 1;
    case UiAction::selectScopePending: return 2;
    case UiAction::selectScopeFailed: return 3;
    case UiAction::selectScopeArchive: return 4;
    case UiAction::selectScopeTrash: return 5;
    default: return -1;
  }
}

inline void reconcileMissingCapsule(UiState &state) {
  state.capsuleDetail = false;
  state.detailMoreOverlay = false;
  state.purgeConfirmOverlay = false;
  state.detailRetryEnabled = false;
  state.detailTrashEnabled = false;
}

inline UiAction uiActionAt(const UiState &state, int16_t x, int16_t y,
                           bool bluetoothEnabled) {
  if (x < 0 || x >= kDisplayWidth || y < 0 || y >= kDisplayHeight) {
    return UiAction::none;
  }
  const UiScreen screen = state.screen();
  if (screen == UiScreen::computerSync) {
    if (x < ui::kBackTargetSize && y < ui::kBackTargetSize) {
      return UiAction::back;
    }
    if (y >= ui::kComputerSyncCloseTop &&
        y < ui::kComputerSyncCloseBottom) {
      return UiAction::closeComputerSync;
    }
    return UiAction::none;
  }
  if (screen == UiScreen::bluetoothPairing) {
    if (x < ui::kBackTargetSize && y < ui::kBackTargetSize) {
      return UiAction::back;
    }
    if (y >= ui::kBluetoothPairTop && y < ui::kBluetoothPairBottom) {
      return bluetoothEnabled ? UiAction::toggleBluetoothPairing
                              : UiAction::none;
    }
    if (y >= ui::kBluetoothForgetTop && y < ui::kBluetoothForgetBottom) {
      return UiAction::forgetBluetoothMac;
    }
    return UiAction::none;
  }
  if (screen == UiScreen::provisioning) {
    const bool backButton = x < ui::kBackTargetSize &&
        y < ui::kBackTargetSize;
    if (backButton) return UiAction::back;
    if (y >= ui::kProvisionExitTop && y < ui::kProvisionExitBottom) {
      return x < ui::kProvisionLogSplit ? UiAction::openProvisioningLog
                                        : UiAction::back;
    }
    return UiAction::none;
  }
  if (screen == UiScreen::provisioningLog) {
    return x < ui::kBackTargetSize && y < ui::kBackTargetSize
        ? UiAction::back : UiAction::none;
  }
  if (state.capsuleScopeOverlay) {
    if (x < ui::kScopePickerLeft || x >= ui::kScopePickerRight ||
        y < ui::kScopePickerTop || y >= ui::kScopePickerBottom) {
      return UiAction::closeOverlay;
    }
    switch ((y - ui::kScopePickerTop) / ui::kScopePickerRowHeight) {
      case 0: return UiAction::selectScopeInbox;
      case 1: return UiAction::selectScopeFavorites;
      case 2: return UiAction::selectScopePending;
      case 3: return UiAction::selectScopeFailed;
      case 4: return UiAction::selectScopeArchive;
      case 5: return UiAction::selectScopeTrash;
    }
    return UiAction::closeOverlay;
  }
  if (state.purgeConfirmOverlay) {
    if (x < ui::kPurgeConfirmLeft || x >= ui::kPurgeConfirmRight ||
        y < ui::kPurgeConfirmTop || y >= ui::kPurgeConfirmBottom) {
      return UiAction::closeOverlay;
    }
    if (y >= ui::kPurgeConfirmActionsTop) {
      return x < ui::kPurgeConfirmActionSplit ? UiAction::closeOverlay
                                               : UiAction::confirmPurge;
    }
    return UiAction::none;
  }
  if (state.shutdownConfirmOverlay) {
    if (x < ui::kShutdownConfirmLeft || x >= ui::kShutdownConfirmRight ||
        y < ui::kShutdownConfirmTop || y >= ui::kShutdownConfirmBottom) {
      return UiAction::closeOverlay;
    }
    if (y >= ui::kShutdownConfirmActionsTop) {
      return x < ui::kShutdownConfirmActionSplit
          ? UiAction::closeOverlay : UiAction::confirmShutdown;
    }
    return UiAction::none;
  }
  if (state.detailMoreOverlay) {
    if (x < ui::kDetailMoreLeft || x >= ui::kDetailMoreRight ||
        y < ui::kDetailMoreTop || y >= ui::kDetailMoreBottom) {
      return UiAction::closeOverlay;
    }
    const int16_t row =
        (y - ui::kDetailMoreTop) / ui::kDetailMoreRowHeight;
    if (row == 0) {
      return state.detailRetryEnabled ? UiAction::retry : UiAction::none;
    }
    if (!state.detailTrashEnabled) return UiAction::none;
    return state.capsuleTrashScope ? UiAction::requestPurge : UiAction::trash;
  }
  if (state.undoAvailable && x >= 20 && x < 348 &&
      y >= 366 && y < 418) return UiAction::undoTrash;
  if (screen == UiScreen::capsuleDetail) {
    if (x < ui::kBackTargetSize && y < ui::kBackTargetSize) {
      return UiAction::back;
    }
    if (y >= ui::kDetailActionsTop && y < ui::kDetailActionsBottom) {
      if (x < 92) return UiAction::play;
      if (x < 184) return UiAction::favorite;
      if (x < 276) return UiAction::archive;
      return UiAction::openDetailMore;
    }
    return UiAction::none;
  }
  if (screen == UiScreen::home) {
    if (state.homeMode == HomeMode::recording) {
      return y >= ui::kRootContentTop && y < ui::kRootContentBottom
          ? UiAction::capsuleRecord : UiAction::none;
    }
    if (y >= ui::kHomePrimaryTop && y < ui::kHomePrimaryConnectedBottom) {
      return UiAction::capsuleRecord;
    }
    if (y >= ui::kHomeSecondaryTop &&
        y < ui::kHomeSecondaryBottom) return UiAction::wechatVoice;
    (void)bluetoothEnabled;
    return UiAction::none;
  }
  if (screen == UiScreen::device) {
    if (y >= ui::kDeviceWifiTop && y < ui::kDeviceMacTop) {
      return x >= ui::kDeviceBluetoothToggleLeft
          ? UiAction::wifiToggle : UiAction::openProvisioning;
    }
    if (y >= ui::kDeviceMacTop && y < ui::kDeviceStorageTop) {
      return x >= ui::kDeviceBluetoothToggleLeft
          ? UiAction::bluetoothToggle : UiAction::openBluetoothPairing;
    }
    if (y >= ui::kDeviceStorageTop && y < ui::kDeviceRaiseTop) {
      return UiAction::openComputerSync;
    }
    if (y >= ui::kDeviceRaiseTop && y < ui::kDeviceProvisionTop) {
      return UiAction::raiseToWakeToggle;
    }
    if (y >= ui::kDeviceProvisionTop && y < ui::kDeviceRowsBottom) {
      return UiAction::openShutdownConfirm;
    }
    return UiAction::none;
  }
  if (screen == UiScreen::capsules) {
    if (y >= ui::kTopBarHeight && y < ui::kCapsuleListTop) {
      return state.capsuleSelectionMode ? UiAction::back
                                        : UiAction::openCapsuleScope;
    }
    if (state.capsuleSelectionMode &&
        y >= ui::kCapsuleSelectionBarTop &&
        y < ui::kCapsuleSelectionBarBottom) {
      if (x < 123) return UiAction::bulkFavorite;
      if (x < 245) return UiAction::bulkArchive;
      return state.capsuleTrashScope ? UiAction::requestPurge
                                     : UiAction::bulkTrash;
    }
    const int16_t listBottom = state.capsuleSelectionMode
        ? ui::kCapsuleSelectionBarTop : ui::kCapsuleListBottom;
    if (y >= ui::kCapsuleListTop && y < listBottom) {
      return UiAction::openCapsule;
    }
  }
  return UiAction::none;
}

}  // namespace pokepod
