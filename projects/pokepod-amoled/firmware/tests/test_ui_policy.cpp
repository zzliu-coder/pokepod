#include <cassert>

#include "../PokePodAmoled/ButtonPolicy.h"
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
  assert(isBackEdgeSwipe(0, 61));
  assert(isBackEdgeSwipe(ui::kBackEdgeWidth - 1, 100));
  assert(!isBackEdgeSwipe(ui::kBackEdgeWidth, 100));
  assert(!isBackEdgeSwipe(0, 60));
  assert(!isBackEdgeSwipe(200, 100));

  assert(scrolledOffset(0, -41, true, 4) == 4);
  assert(scrolledOffset(0, -41, false, 4) == 0);
  assert(scrolledOffset(4, 41, true, 4) == 0);
  assert(scrolledOffset(0, 41, true, 4) == 0);
  assert(scrolledOffset(4, 20, true, 4) == 4);

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
  assert(uiActionAt(state, 180, 300, false) == UiAction::capsuleRecord);
  assert(uiActionAt(state, 180, 300, true) == UiAction::wechatDictation);
  assert(uiActionAt(state, 180, 430, true) == UiAction::none);
  state.homeMode = HomeMode::recording;
  assert(uiActionAt(state, 180, 300, true) == UiAction::capsuleRecord);
  assert(uiActionAt(state, 180, 430, true) == UiAction::none);

  state.capsuleDetail = true;
  assert(state.screen() == UiScreen::capsuleDetail);
  assert(uiActionAt(state, 20, 20, true) == UiAction::back);
  assert(uiActionAt(state, 80, 20, true) == UiAction::none);
  assert(uiActionAt(state, 20, 370, true) == UiAction::play);
  assert(uiActionAt(state, 120, 370, true) == UiAction::favorite);
  assert(uiActionAt(state, 220, 370, true) == UiAction::archive);
  assert(uiActionAt(state, 320, 370, true) == UiAction::none);
  state.detailRetryEnabled = true;
  assert(uiActionAt(state, 320, 370, true) == UiAction::retry);

  // Provisioning owns the whole screen and masks every underlying device hit.
  state.provisioning = true;
  assert(state.screen() == UiScreen::provisioning);
  assert(uiActionAt(state, 20, 20, true) == UiAction::back);
  assert(uiActionAt(state, 180, 370, true) == UiAction::none);
  state.provisioning = false;
  state.detailRetryEnabled = false;
  state.capsuleDetail = false;
  state.page = RootPage::device;
  state.homeMode = HomeMode::idle;
  assert(uiActionAt(state, 180, 120, false) == UiAction::wifiToggle);
  assert(uiActionAt(state, 180, 300, false) == UiAction::raiseToWakeToggle);
  assert(uiActionAt(state, 180, 380, false) == UiAction::openProvisioning);
  assert(uiActionAt(state, 180, 200, false) == UiAction::none);

  state.page = RootPage::capsules;
  assert(uiActionAt(state, 180, ui::kCapsuleListTop, false) ==
         UiAction::openCapsule);
  assert(uiActionAt(state, 180, ui::kCapsuleListBottom - 1, false) ==
         UiAction::openCapsule);
  assert(uiActionAt(state, 180, ui::kCapsuleListBottom, false) ==
         UiAction::none);

  assert(bootGestureAction(false, 100) == BootGestureAction::capsuleToggle);
  assert(bootGestureAction(true, 100) == BootGestureAction::dictationRelease);
  assert(bootGestureAction(true, 900) == BootGestureAction::dictationRelease);
  assert(bootGestureAction(true, 10) == BootGestureAction::none);
  assert(!bootPressStartsDictation(false));
  assert(bootPressStartsDictation(true));
  return 0;
}
