#pragma once

#include <stdint.h>

namespace pokepod {

class ButtonDebouncer {
 public:
  explicit ButtonDebouncer(uint32_t debounceMs = 35)
      : debounceMs_(debounceMs) {}

  bool update(bool pressed, uint32_t nowMs) {
    pressedEdge_ = false;
    releasedEdge_ = false;
    if (pressed != candidate_) {
      candidate_ = pressed;
      candidateSinceMs_ = nowMs;
      return false;
    }
    if (candidate_ != stable_ && nowMs - candidateSinceMs_ >= debounceMs_) {
      stable_ = candidate_;
      pressedEdge_ = stable_;
      releasedEdge_ = !stable_;
      return true;
    }
    return false;
  }

  bool pressedEdge() const { return pressedEdge_; }
  bool releasedEdge() const { return releasedEdge_; }
  bool pressed() const { return stable_; }

 private:
  uint32_t debounceMs_;
  uint32_t candidateSinceMs_ = 0;
  bool candidate_ = false;
  bool stable_ = false;
  bool pressedEdge_ = false;
  bool releasedEdge_ = false;
};

}  // namespace pokepod

