#pragma once

#include <stdint.h>

namespace pokepod {

constexpr uint32_t kLongPressMs = 800;

enum class BootGestureAction {
  none,
  capsuleToggle,
  dictationToggle,
};

inline BootGestureAction bootGestureAction(bool macConnected, uint32_t heldMs) {
  if (heldMs < 25) return BootGestureAction::none;
  if (heldMs >= kLongPressMs) return BootGestureAction::capsuleToggle;
  return macConnected ? BootGestureAction::dictationToggle
                      : BootGestureAction::capsuleToggle;
}

}  // namespace pokepod
