#include <cassert>

#include "../PokePodAmoled/UiPolicy.h"

int main() {
  using namespace pokepod;
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

  DeviceHealthState health = {
      true, true, true, true, true, true, true, true, true, true};
  assert(health.ready());
  health.touch = false;
  assert(!health.ready());
  health.touch = true;
  health.pmu = false;
  assert(!health.ready());
  health.pmu = true;
  health.fullTextFont = false;
  assert(!health.ready());

  UiState state;
  assert(state.screen() == UiScreen::home);
  assert(uiActionAt(state, 180, 180, false) == UiAction::capsuleRecord);
  assert(uiActionAt(state, 180, 300, false) == UiAction::wechatVoice);
  assert(uiActionAt(state, 180, 300, true) == UiAction::wechatVoice);
  assert(uiActionAt(state, 180, 430, true) == UiAction::none);
  state.homeMode = HomeMode::recording;
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
  state.undoAvailable = false;
  state.detailRetryEnabled = false;
  state.capsuleDetail = false;
  state.page = RootPage::device;
  state.homeMode = HomeMode::idle;
  assert(uiActionAt(state, 180, 120, false) == UiAction::wifiToggle);
  assert(uiActionAt(state, 180, 300, false) == UiAction::raiseToWakeToggle);
  assert(uiActionAt(state, 180, 380, false) == UiAction::openProvisioning);
  assert(uiActionAt(state, 180, 200, false) == UiAction::wirelessSettings);
  assert(uiActionAt(state, 180, 240, false) == UiAction::toggleComputerSync);

  state.page = RootPage::capsules;
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
         UiAction::none);
  state.undoAvailable = true;
  assert(uiActionAt(state, 180, 390, false) == UiAction::undoTrash);

  UiState missingDetail;
  missingDetail.capsuleDetail = true;
  missingDetail.detailMoreOverlay = true;
  missingDetail.detailRetryEnabled = true;
  missingDetail.detailTrashEnabled = true;
  reconcileMissingCapsule(missingDetail);
  assert(!missingDetail.capsuleDetail);
  assert(!missingDetail.detailMoreOverlay);
  assert(!missingDetail.detailRetryEnabled);
  assert(!missingDetail.detailTrashEnabled);
  assert(uiActionAt(missingDetail, 180, ui::kDetailMoreTop + 10, false) ==
         UiAction::capsuleRecord);

  return 0;
}
