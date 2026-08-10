#pragma once

#include <stdint.h>

namespace pokepod {

struct ScrollFrameReusePlan {
  bool valid = false;
  int16_t sourceRow = 0;
  int16_t destinationRow = 0;
  int16_t moveRows = 0;
  int16_t exposedRow = 0;
  int16_t exposedRows = 0;
};

inline ScrollFrameReusePlan scrollFrameReusePlan(int32_t previous,
                                                  int32_t current,
                                                  int16_t viewportRows) {
  ScrollFrameReusePlan plan;
  const int32_t delta = current - previous;
  if (viewportRows <= 0 || delta <= -viewportRows || delta >= viewportRows) {
    return plan;
  }
  plan.valid = true;
  if (delta > 0) {
    plan.sourceRow = static_cast<int16_t>(delta);
    plan.moveRows = static_cast<int16_t>(viewportRows - delta);
    plan.exposedRow = plan.moveRows;
    plan.exposedRows = static_cast<int16_t>(delta);
  } else if (delta < 0) {
    plan.destinationRow = static_cast<int16_t>(-delta);
    plan.moveRows = static_cast<int16_t>(viewportRows + delta);
    plan.exposedRows = static_cast<int16_t>(-delta);
  } else {
    plan.moveRows = viewportRows;
  }
  return plan;
}

}  // namespace pokepod
