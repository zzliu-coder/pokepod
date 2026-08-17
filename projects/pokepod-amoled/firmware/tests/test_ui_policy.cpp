#include <cassert>

#include "../PokePodAmoled/UiPolicy.h"

int main() {
  using namespace pokepod;
  assert(!uiNoticeUsesErrorIcon(UiNoticeKind::info));
  assert(!uiNoticeUsesErrorIcon(UiNoticeKind::progress));
  assert(!uiNoticeUsesErrorIcon(UiNoticeKind::warning));
  assert(uiNoticeUsesErrorIcon(UiNoticeKind::error));
  assert(uiNoticeUsesWarningIcon(UiNoticeKind::warning));
  assert(!uiNoticeUsesWarningIcon(UiNoticeKind::info));
  assert(!uiNoticeUsesWarningIcon(UiNoticeKind::error));
  assert(uiNoticeUsesCheckIcon(UiNoticeKind::success));
  assert(!uiNoticeUsesCheckIcon(UiNoticeKind::progress));
  assert(uiNoticeIsProgress(UiNoticeKind::progress));
  assert(!uiNoticeIsProgress(UiNoticeKind::error));

  // Physical BOOT contract: a short press controls local recording, while a
  // held press is consumed by wireless voice (including a failed start).
  BootGesturePolicy boot;
  BootGestureContext bootContext;
  assert(boot.pressed(100, bootContext) == BootGestureAction::none);
  assert(boot.held(100 + ui::kWirelessHoldDelayMs - 1, bootContext) ==
         BootGestureAction::none);
  assert(boot.held(100 + ui::kWirelessHoldDelayMs, bootContext) ==
         BootGestureAction::wirelessUnavailable);
  assert(boot.released(bootContext) == BootGestureAction::none);

  bootContext.wirelessAppReady = true;
  assert(boot.pressed(1000, bootContext) == BootGestureAction::none);
  assert(boot.held(1000 + ui::kWirelessHoldDelayMs - 1, bootContext) ==
         BootGestureAction::none);
  assert(boot.held(1000 + ui::kWirelessHoldDelayMs, bootContext) ==
         BootGestureAction::startWirelessVoice);
  assert(boot.held(1000 + ui::kWirelessHoldDelayMs + 1, bootContext) ==
         BootGestureAction::none);
  bootContext.wirelessHolding = true;
  assert(boot.released(bootContext) == BootGestureAction::stopWirelessVoice);

  bootContext.wirelessHolding = false;
  bootContext.wirelessAppReady = false;
  assert(boot.pressed(2000, bootContext) == BootGestureAction::none);
  assert(boot.released(bootContext) == BootGestureAction::startLocalRecording);
  bootContext.localRecording = true;
  assert(boot.pressed(3000, bootContext) == BootGestureAction::none);
  assert(boot.released(bootContext) == BootGestureAction::stopLocalRecording);

  // A ready Mac voice session reserves the physical button's short release
  // for the "请按住说话" hint; local capsules remain screen-controlled.
  bootContext.localRecording = false;
  bootContext.wirelessAppReady = true;
  assert(boot.pressed(3500, bootContext) == BootGestureAction::none);
  assert(boot.released(bootContext) ==
         BootGestureAction::voiceReadyShortPress);

  bootContext.localRecording = true;
  assert(boot.pressed(3600, bootContext) == BootGestureAction::none);
  assert(boot.released(bootContext) == BootGestureAction::stopLocalRecording);

  bootContext.localRecording = false;
  bootContext.screenOn = false;
  assert(boot.pressed(4000, bootContext) == BootGestureAction::wakeScreen);
  bootContext.screenOn = true;
  assert(boot.released(bootContext) == BootGestureAction::none);

  bootContext.provisioning = true;
  bootContext.sensitiveConfirmationPending = true;
  assert(boot.pressed(5000, bootContext) ==
         BootGestureAction::confirmProvisioning);
  assert(boot.released(bootContext) == BootGestureAction::none);
  bootContext.sensitiveConfirmationPending = false;
  assert(boot.pressed(6000, bootContext) ==
         BootGestureAction::armProvisioningExit);
  assert(boot.released(bootContext) == BootGestureAction::exitProvisioning);

  bootContext.provisioning = false;
  bootContext.bluetoothEnabled = false;
  assert(boot.pressed(7000, bootContext) == BootGestureAction::none);
  assert(boot.held(7000 + ui::kWirelessHoldDelayMs, bootContext) ==
         BootGestureAction::bluetoothDisabled);
  assert(boot.released(bootContext) == BootGestureAction::none);

  assert(static_cast<int>(RootPage::capsules) == 0);
  assert(static_cast<int>(RootPage::home) == 1);
  assert(static_cast<int>(RootPage::device) == 2);
  assert(swipedPage(RootPage::home, 80, false) == RootPage::capsules);
  assert(swipedPage(RootPage::home, -80, false) == RootPage::device);
  assert(swipedPage(RootPage::capsules, 80, false) == RootPage::capsules);
  assert(swipedPage(RootPage::capsules, -80, false) == RootPage::home);
  assert(swipedPage(RootPage::device, 80, false) == RootPage::home);
  assert(swipedPage(RootPage::device, -80, false) == RootPage::device);
  assert(swipedPage(RootPage::home, -100, true) == RootPage::home);
  assert(touchTapEligible(0, 0));
  assert(touchTapEligible(ui::kTouchTapSlop, -ui::kTouchTapSlop));
  assert(!touchTapEligible(ui::kTouchTapSlop + 1, 0));
  assert(!touchTapEligible(0, -(ui::kTouchTapSlop + 1)));
  assert(touchHorizontalSwipe(ui::kTouchSwipeThreshold, 10));
  assert(touchHorizontalSwipe(-ui::kTouchSwipeThreshold, 10));
  assert(!touchHorizontalSwipe(ui::kTouchSwipeThreshold - 1, 0));
  assert(!touchHorizontalSwipe(ui::kTouchSwipeThreshold, 80));
  assert(touchVerticalSwipe(10, ui::kTouchVerticalThreshold));
  assert(!touchVerticalSwipe(0, ui::kTouchVerticalThreshold - 1));
  assert(!wirelessHoldReady(ui::kWirelessHoldDelayMs - 1, 0, 0));
  assert(wirelessHoldReady(ui::kWirelessHoldDelayMs, 0, 0));
  assert(!wirelessHoldReady(ui::kWirelessHoldDelayMs,
                            ui::kTouchTapSlop + 1, 0));

  TouchGestureTracker gesture;
  gesture.begin(180, 300, 1000);
  gesture.update(184, 303);
  assert(gesture.tapEligible());
  assert(!gesture.wirelessHoldReady(1000 + ui::kWirelessHoldDelayMs - 1));
  assert(gesture.wirelessHoldReady(1000 + ui::kWirelessHoldDelayMs));
  gesture.update(180 - ui::kTouchSwipeThreshold, 304);
  assert(gesture.horizontalSwipe());
  assert(!gesture.tapEligible());
  assert(!gesture.wirelessHoldReady(2000));
  gesture.reset();
  assert(!gesture.active);

  gesture.begin(180, 180, 2000);
  gesture.update(180 + ui::kTouchTapSlop + 1, 180);
  gesture.update(180, 180);
  assert(!gesture.tapEligible());
  assert(!gesture.horizontalSwipe());
  assert(isBackEdgeSwipe(0, ui::kTouchSwipeThreshold + 1));
  assert(isBackEdgeSwipe(ui::kBackEdgeWidth - 1, 100));
  assert(!isBackEdgeSwipe(ui::kBackEdgeWidth, 100));
  assert(!isBackEdgeSwipe(0, ui::kTouchSwipeThreshold));
  assert(!isBackEdgeSwipe(200, 100));

  DeviceHealthState health;
  health.ioExpander = health.display = health.touch = health.sdCard = true;
  health.rtc = health.imu = health.pmu = health.audio = health.usb = true;
  health.capsuleLibrary = true;
  health.recorder = health.bleVoice = health.link = health.wifi = true;
  health.transcription = true;
  health.fullTextFont = true;
  assert(health.ready());
  health.touch = false;
  assert(!health.ready());
  health.touch = true;
  health.pmu = false;
  assert(!health.ready());
  health.pmu = true;
  health.fullTextFont = false;
  assert(!health.ready());

  assert(uiActionRequiresCapsuleLibrary(UiAction::openCapsule));
  assert(uiActionRequiresCapsuleLibrary(UiAction::play));
  assert(uiActionRequiresCapsuleLibrary(UiAction::bulkTrash));
  assert(!uiActionRequiresCapsuleLibrary(UiAction::capsuleRecord));
  assert(!uiActionRequiresCapsuleLibrary(UiAction::wechatVoice));

  UiState state;
  assert(state.screen() == UiScreen::home);
  assert(uiActionAt(state, 180, 20, false) == UiAction::none);
  assert(uiActionAt(state, 180, 180, false) == UiAction::capsuleRecord);
  assert(uiActionAt(state, 180, 300, false) == UiAction::wechatVoice);
  assert(uiActionAt(state, 180, 300, true) == UiAction::wechatVoice);
  assert(uiActionAt(state, 180, 430, true) == UiAction::none);
  state.homeMode = HomeMode::recording;
  assert(uiActionAt(state, 180, 20, true) == UiAction::none);
  assert(uiActionAt(state, 180, 300, true) == UiAction::capsuleRecord);
  assert(uiActionAt(state, 180, 430, true) == UiAction::none);
  state.homeMode = HomeMode::transcribing;
  assert(uiActionAt(state, 180, 180, true) == UiAction::capsuleRecord);
  assert(uiActionAt(state, 180, 300, true) == UiAction::wechatVoice);

  state.capsuleDetail = true;
  assert(state.screen() == UiScreen::capsuleDetail);
  assert(uiActionAt(state, 20, 20, true) == UiAction::back);
  assert(uiActionAt(state, 80, 20, true) == UiAction::none);
  assert(uiActionAt(state, 20, 370, true) == UiAction::play);
  assert(uiActionAt(state, 120, 370, true) == UiAction::favorite);
  assert(uiActionAt(state, 220, 370, true) == UiAction::archive);
  assert(uiActionAt(state, 320, 370, true) == UiAction::openDetailMore);
  state.detailMoreOverlay = true;
  state.undoAvailable = true;
  assert(uiActionAt(state, 20, 370, true) == UiAction::closeOverlay);
  assert(uiActionAt(state, 180, ui::kDetailMoreTop + 10, true) ==
         UiAction::none);
  assert(uiActionAt(state, 180,
                    ui::kDetailMoreTop + ui::kDetailMoreRowHeight + 10,
                    true) == UiAction::none);
  state.detailRetryEnabled = true;
  state.detailTrashEnabled = true;
  assert(uiActionAt(state, 180, ui::kDetailMoreTop + 10, true) ==
         UiAction::retry);
  assert(uiActionAt(state, 180,
                    ui::kDetailMoreTop + ui::kDetailMoreRowHeight + 10,
                    true) == UiAction::trash);
  state.capsuleTrashScope = true;
  assert(uiActionAt(state, 180,
                    ui::kDetailMoreTop + ui::kDetailMoreRowHeight + 10,
                    true) == UiAction::requestPurge);
  state.detailMoreOverlay = false;
  state.purgeConfirmOverlay = true;
  assert(uiActionAt(state, 100, ui::kPurgeConfirmActionsTop + 10, true) ==
         UiAction::closeOverlay);
  assert(uiActionAt(state, 260, ui::kPurgeConfirmActionsTop + 10, true) ==
         UiAction::confirmPurge);
  assert(uiActionAt(state, 10, 100, true) == UiAction::closeOverlay);
  state.purgeConfirmOverlay = false;
  state.capsuleTrashScope = false;

  // Provisioning owns the whole screen and masks every underlying device hit.
  state.provisioning = true;
  assert(state.screen() == UiScreen::provisioning);
  assert(uiActionAt(state, 20, 20, true) == UiAction::back);
  assert(uiActionAt(state, 180, ui::kProvisionExitTop - 1, true) ==
         UiAction::none);
  assert(uiActionAt(state, 100, ui::kProvisionExitTop, true) ==
         UiAction::openProvisioningLog);
  assert(uiActionAt(state, 250, 390, true) == UiAction::back);
  assert(uiActionAt(state, 250, ui::kProvisionExitBottom - 1, true) ==
         UiAction::back);
  state.provisioningLog = true;
  assert(state.screen() == UiScreen::provisioningLog);
  assert(uiActionAt(state, 20, 20, true) == UiAction::back);
  assert(uiActionAt(state, 180, 390, true) == UiAction::none);
  state.provisioningLog = false;
  state.provisioning = false;
  state.detailMoreOverlay = false;
  state.purgeConfirmOverlay = false;
  state.undoAvailable = false;
  state.detailRetryEnabled = false;
  state.capsuleDetail = false;
  state.page = RootPage::device;
  state.homeMode = HomeMode::idle;
  assert(uiActionAt(state, 180, 120, false) == UiAction::openProvisioning);
  assert(uiActionAt(state, ui::kDeviceBluetoothToggleLeft, 120, false) ==
         UiAction::wifiToggle);
  assert(uiActionAt(state, 180, 300, false) == UiAction::raiseToWakeToggle);
  assert(uiActionAt(state, 180, 380, false) == UiAction::openShutdownConfirm);
  assert(uiActionAt(state, 180, 200, false) ==
         UiAction::openBluetoothPairing);
  assert(uiActionAt(state, 330, 200, false) ==
         UiAction::bluetoothToggle);
  assert(uiActionAt(state, 180, 240, false) == UiAction::openComputerSync);
  state.shutdownConfirmOverlay = true;
  assert(uiActionAt(state, 100, ui::kShutdownConfirmActionsTop + 10, false) ==
         UiAction::closeOverlay);
  assert(uiActionAt(state, 260, ui::kShutdownConfirmActionsTop + 10, false) ==
         UiAction::confirmShutdown);
  assert(uiActionAt(state, 10, 100, false) == UiAction::closeOverlay);
  state.shutdownConfirmOverlay = false;

  assert(uiActionAt(state, 180, 20, false) == UiAction::none);
  assert(uiActionAt(state, 338, 20, false) == UiAction::none);
  state.computerSync = true;
  assert(state.screen() == UiScreen::computerSync);
  assert(uiActionAt(state, 20, 20, false) == UiAction::back);
  assert(uiActionAt(state, 180, ui::kComputerSyncCloseTop, false) ==
         UiAction::closeComputerSync);
  assert(uiActionAt(state, 180, ui::kComputerSyncCloseBottom, false) ==
         UiAction::none);
  assert(uiActionAt(state, 180, 20, false) == UiAction::none);
  state.computerSync = false;

  state.bluetoothPairing = true;
  assert(state.screen() == UiScreen::bluetoothPairing);
  assert(uiActionAt(state, 20, 20, false) == UiAction::back);
  assert(uiActionAt(state, 180, ui::kBluetoothPairTop, true) ==
         UiAction::toggleBluetoothPairing);
  assert(uiActionAt(state, 180, ui::kBluetoothPairTop, false) ==
         UiAction::none);
  assert(uiActionAt(state, 180, ui::kBluetoothPairBottom, false) ==
         UiAction::none);
  assert(uiActionAt(state, 180, ui::kBluetoothForgetTop, false) ==
         UiAction::forgetBluetoothMac);
  assert(uiActionAt(state, 180, ui::kBluetoothForgetBottom, false) ==
         UiAction::none);
  state.bluetoothPairing = false;

  state.page = RootPage::capsules;
  assert(uiActionAt(state, 180, 20, false) == UiAction::none);
  assert(uiActionAt(state, 180, 70, false) == UiAction::openCapsuleScope);
  state.capsuleScopeOverlay = true;
  state.undoAvailable = true;
  assert(uiActionAt(state, 10, 200, false) == UiAction::closeOverlay);
  assert(uiActionAt(state, 180, ui::kScopePickerTop + 5, false) ==
         UiAction::selectScopeInbox);
  assert(uiActionAt(state, 180,
                    ui::kScopePickerTop + ui::kScopePickerRowHeight + 5,
                    false) == UiAction::selectScopeFavorites);
  assert(uiActionAt(state, 180,
                    ui::kScopePickerTop + ui::kScopePickerRowHeight * 5 + 5,
                    false) == UiAction::selectScopeTrash);
  assert(capsuleScopeIndexForAction(UiAction::selectScopeInbox) == 0);
  assert(capsuleScopeIndexForAction(UiAction::selectScopeTrash) == 5);
  assert(capsuleScopeIndexForAction(UiAction::closeOverlay) == -1);
  state.capsuleScopeOverlay = false;
  state.undoAvailable = false;
  assert(uiActionAt(state, 180, ui::kCapsuleListTop, false) ==
         UiAction::openCapsule);
  assert(uiActionAt(state, 180, ui::kCapsuleListBottom - 1, false) ==
         UiAction::openCapsule);
  assert(uiActionAt(state, 180, ui::kCapsuleListBottom, false) ==
         UiAction::none);
  state.capsuleSelectionMode = true;
  assert(uiActionAt(state, 180, 70, false) == UiAction::back);
  assert(uiActionAt(state, 60, ui::kCapsuleSelectionBarTop, false) ==
         UiAction::bulkFavorite);
  assert(uiActionAt(state, 180, ui::kCapsuleSelectionBarTop, false) ==
         UiAction::bulkArchive);
  assert(uiActionAt(state, 300, ui::kCapsuleSelectionBarTop, false) ==
         UiAction::bulkTrash);
  state.capsuleTrashScope = true;
  assert(uiActionAt(state, 300, ui::kCapsuleSelectionBarTop, false) ==
         UiAction::requestPurge);
  state.undoAvailable = true;
  assert(uiActionAt(state, 180, 390, false) == UiAction::undoTrash);

  UiState missingDetail;
  missingDetail.capsuleDetail = true;
  missingDetail.detailMoreOverlay = true;
  missingDetail.purgeConfirmOverlay = true;
  missingDetail.detailRetryEnabled = true;
  missingDetail.detailTrashEnabled = true;
  reconcileMissingCapsule(missingDetail);
  assert(!missingDetail.capsuleDetail);
  assert(!missingDetail.detailMoreOverlay);
  assert(!missingDetail.purgeConfirmOverlay);
  assert(!missingDetail.detailRetryEnabled);
  assert(!missingDetail.detailTrashEnabled);
  assert(uiActionAt(missingDetail, 180, ui::kDetailMoreTop + 10, false) ==
         UiAction::capsuleRecord);

  return 0;
}
