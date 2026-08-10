#pragma once

#include <stdint.h>

#include "UiTheme.h"

namespace pokepod {

struct UiRenderPlan {
  bool composeRecordingBeforeFullPresent = false;
  bool presentRecordingAsPartial = false;
};

inline UiRenderPlan uiRenderPlan(bool recording, bool bodyRepainted) {
  UiRenderPlan plan;
  plan.composeRecordingBeforeFullPresent = recording && bodyRepainted;
  plan.presentRecordingAsPartial = recording && !bodyRepainted;
  return plan;
}

inline bool shouldDrawToast(bool hasMessage, bool recording,
                            bool wirelessHolding, bool provisioning) {
  return hasMessage && !recording && !wirelessHolding && !provisioning;
}

enum class UiScrollSurface : uint8_t {
  none,
  capsules,
  detail,
  provisioningLog,
};

struct UiPresentRegion {
  int16_t x = 0;
  int16_t y = 0;
  int16_t width = 0;
  int16_t height = 0;

  bool valid() const { return width > 0 && height > 0; }
};

inline UiPresentRegion scrollPresentRegion(UiScrollSurface surface,
                                           bool selectionMode) {
  switch (surface) {
    case UiScrollSurface::capsules: {
      const int16_t bottom = selectionMode
          ? ui::kCapsuleSelectionBarTop : ui::kCapsuleListBottom;
      return {0, ui::kCapsuleListTop, ui::kScreenWidth,
              static_cast<int16_t>(bottom - ui::kCapsuleListTop)};
    }
    case UiScrollSurface::detail:
      return {0, ui::kDetailTextTop, ui::kScreenWidth,
              static_cast<int16_t>(ui::kDetailTextBottom - ui::kDetailTextTop)};
    case UiScrollSurface::provisioningLog:
      return {0, ui::kProvisionLogListTop, ui::kScreenWidth,
              static_cast<int16_t>(ui::kProvisionLogListBottom -
                                   ui::kProvisionLogListTop)};
    case UiScrollSurface::none: break;
  }
  return {};
}

}  // namespace pokepod
