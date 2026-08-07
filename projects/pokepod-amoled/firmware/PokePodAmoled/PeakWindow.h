#pragma once

#include <stdint.h>

namespace pokepod {

class PeakWindow {
 public:
  void observe(uint16_t sample) {
    latest_ = sample;
    if (sample > window_) window_ = sample;
  }

  uint16_t consume() {
    const uint16_t value = window_;
    window_ = 0;
    return value;
  }

  void reset() {
    latest_ = 0;
    window_ = 0;
  }

  uint16_t latest() const { return latest_; }

 private:
  uint16_t latest_ = 0;
  uint16_t window_ = 0;
};

}  // namespace pokepod
