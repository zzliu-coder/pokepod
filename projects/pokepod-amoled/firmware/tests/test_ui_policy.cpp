#include <cassert>

#include "../PokePodAmoled/ButtonPolicy.h"
#include "../PokePodAmoled/UiPolicy.h"

int main() {
  using namespace pokepod;
  assert(static_cast<int>(RootPage::home) == 0);
  assert(swipedPage(RootPage::home, 80, false) == RootPage::home);
  assert(swipedPage(RootPage::home, -80, false) == RootPage::capsules);
  assert(swipedPage(RootPage::capsules, 80, false) == RootPage::home);
  assert(swipedPage(RootPage::capsules, -80, false) == RootPage::device);
  assert(swipedPage(RootPage::device, 80, false) == RootPage::capsules);
  assert(swipedPage(RootPage::device, -80, false) == RootPage::device);
  assert(swipedPage(RootPage::home, -100, true) == RootPage::home);

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
  assert(uiActionAt(state, 180, 200, false) == UiAction::capsuleRecord);
  assert(uiActionAt(state, 180, 370, false) == UiAction::none);
  assert(uiActionAt(state, 180, 370, true) == UiAction::wechatDictation);
  assert(uiActionAt(state, 40, 424, true) == UiAction::goHome);
  assert(uiActionAt(state, 184, 424, true) == UiAction::goCapsules);
  assert(uiActionAt(state, 330, 424, true) == UiAction::goDevice);
  state.homeMode = HomeMode::recording;
  assert(uiActionAt(state, 180, 340, true) == UiAction::capsuleRecord);
  assert(uiActionAt(state, 180, 424, true) == UiAction::none);
  state.capsuleDetail = true;
  assert(uiActionAt(state, 20, 20, true) == UiAction::back);
  assert(uiActionAt(state, 20, 360, true) == UiAction::play);
  assert(uiActionAt(state, 120, 360, true) == UiAction::favorite);
  assert(uiActionAt(state, 220, 360, true) == UiAction::archive);
  assert(uiActionAt(state, 320, 360, true) == UiAction::none);
  state.detailRetryEnabled = true;
  assert(uiActionAt(state, 320, 360, true) == UiAction::retry);
  state.detailRetryEnabled = false;
  state.capsuleDetail = false;
  state.page = RootPage::device;
  state.homeMode = HomeMode::idle;
  assert(uiActionAt(state, 180, 130, false) == UiAction::wifiToggle);
  assert(uiActionAt(state, 180, 300, false) == UiAction::raiseToWakeToggle);
  assert(uiActionAt(state, 180, 360, false) == UiAction::openProvisioning);
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
