#include <assert.h>

#include "UiRenderPolicy.h"

int main() {
  using namespace pokepod;

  UiRenderPlan plan = uiRenderPlan(true, true);
  assert(plan.composeRecordingBeforeFullPresent);
  assert(!plan.presentRecordingAsPartial);

  plan = uiRenderPlan(true, false);
  assert(!plan.composeRecordingBeforeFullPresent);
  assert(plan.presentRecordingAsPartial);

  plan = uiRenderPlan(false, true);
  assert(!plan.composeRecordingBeforeFullPresent);
  assert(!plan.presentRecordingAsPartial);

  assert(!shouldDrawToast(true, true, false, false));
  assert(!shouldDrawToast(true, false, true, false));
  assert(!shouldDrawToast(true, false, false, true));
  assert(shouldDrawToast(true, false, false, false));
  assert(!shouldDrawToast(false, false, false, false));
  return 0;
}
