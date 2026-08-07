#include <cassert>

#include "../PokePodAmoled/ButtonPolicy.h"
#include "../PokePodAmoled/UiPolicy.h"

int main() {
  using namespace pokepod;
  assert(swipedPage(RootPage::home, 80, false) == RootPage::capsules);
  assert(swipedPage(RootPage::home, -80, false) == RootPage::device);
  assert(swipedPage(RootPage::capsules, 80, false) == RootPage::capsules);
  assert(swipedPage(RootPage::device, -80, false) == RootPage::device);
  assert(swipedPage(RootPage::home, -100, true) == RootPage::home);

  UiState state;
  assert(uiActionAt(state, 180, 200, false) == UiAction::capsuleRecord);
  assert(uiActionAt(state, 180, 320, false) == UiAction::none);
  assert(uiActionAt(state, 180, 320, true) == UiAction::wechatDictation);
  state.homeMode = HomeMode::recording;
  assert(uiActionAt(state, 180, 340, true) == UiAction::capsuleRecord);
  state.capsuleDetail = true;
  assert(uiActionAt(state, 20, 20, true) == UiAction::back);
  assert(uiActionAt(state, 20, 390, true) == UiAction::play);
  assert(uiActionAt(state, 120, 390, true) == UiAction::favorite);
  assert(uiActionAt(state, 220, 390, true) == UiAction::archive);
  assert(uiActionAt(state, 320, 390, true) == UiAction::retry);
  state.capsuleDetail = false;
  state.page = RootPage::device;
  state.homeMode = HomeMode::idle;
  assert(uiActionAt(state, 180, 205, false) == UiAction::wifiToggle);
  assert(uiActionAt(state, 180, 290, false) == UiAction::openProvisioning);
  assert(uiActionAt(state, 180, 370, false) == UiAction::raiseToWakeToggle);

  assert(bootGestureAction(false, 100) == BootGestureAction::capsuleToggle);
  assert(bootGestureAction(true, 100) == BootGestureAction::dictationRelease);
  assert(bootGestureAction(true, 900) == BootGestureAction::dictationRelease);
  assert(bootGestureAction(true, 10) == BootGestureAction::none);
  assert(!bootPressStartsDictation(false));
  assert(bootPressStartsDictation(true));
  return 0;
}
