#pragma once

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

}  // namespace pokepod
