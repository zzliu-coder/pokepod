#pragma once

#include <stdint.h>

namespace pokepod {

enum class BootGestureAction {
  none,
  capsuleToggle,
  dictationRelease,
};

inline BootGestureAction bootGestureAction(bool macConnected, uint32_t heldMs) {
  if (heldMs < 25) return BootGestureAction::none;
  return macConnected ? BootGestureAction::dictationRelease
                      : BootGestureAction::capsuleToggle;
}

inline bool bootPressStartsDictation(bool macConnected) {
  return macConnected;
}

}  // namespace pokepod
