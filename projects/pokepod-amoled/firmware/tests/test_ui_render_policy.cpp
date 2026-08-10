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

  UiPresentRegion region = scrollPresentRegion(
      UiScrollSurface::capsules, false);
  assert(region.valid());
  assert(region.y == ui::kCapsuleListTop);
  assert(region.height == ui::kCapsuleListBottom - ui::kCapsuleListTop);

  region = scrollPresentRegion(UiScrollSurface::capsules, true);
  assert(region.height ==
         ui::kCapsuleSelectionBarTop - ui::kCapsuleListTop);

  region = scrollPresentRegion(UiScrollSurface::detail, false);
  assert(region.y == ui::kDetailTextTop);
  assert(region.height == ui::kDetailTextBottom - ui::kDetailTextTop);

  region = scrollPresentRegion(UiScrollSurface::provisioningLog, false);
  assert(region.y == ui::kProvisionLogListTop);
  assert(region.height ==
         ui::kProvisionLogListBottom - ui::kProvisionLogListTop);

  assert(!scrollPresentRegion(UiScrollSurface::none, false).valid());
  return 0;
}
