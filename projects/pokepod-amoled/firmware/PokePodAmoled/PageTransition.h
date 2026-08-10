#pragma once

#include <stdint.h>

namespace pokepod {

enum class PageTransitionDirection : uint8_t { none, fromLeft, fromRight };

struct PageTransitionRegion {
  int16_t x = 0;
  int16_t width = 0;
  bool valid() const { return width > 0; }
};

// Non-blocking page reveal. The new page is composed once in the indexed
// canvas, then narrow strips replace the old panel contents over 140 ms.
class PageTransition {
 public:
  static constexpr uint32_t kDurationMs = 140;
  static constexpr int16_t kInitialRevealPx = 32;

  void prepare(PageTransitionDirection direction) {
    if (direction == PageTransitionDirection::none) return;
    direction_ = direction;
    pending_ = true;
    active_ = false;
    revealedPx_ = 0;
  }

  PageTransitionRegion begin(uint32_t nowMs, int16_t screenWidth) {
    if (!pending_ || screenWidth <= 0) return {};
    pending_ = false;
    active_ = true;
    startedAtMs_ = nowMs;
    screenWidth_ = screenWidth;
    return revealTo(screenWidth < kInitialRevealPx
                        ? screenWidth : kInitialRevealPx);
  }

  PageTransitionRegion advance(uint32_t nowMs) {
    if (!active_) return {};
    const uint32_t elapsed = static_cast<uint32_t>(nowMs - startedAtMs_);
    int32_t target = elapsed >= kDurationMs ? screenWidth_ :
        static_cast<int32_t>(screenWidth_) * elapsed / kDurationMs;
    if (target < revealedPx_) target = revealedPx_;
    if (target == revealedPx_ && target < screenWidth_) return {};
    return revealTo(static_cast<int16_t>(target));
  }

  void cancel() {
    pending_ = false;
    active_ = false;
    direction_ = PageTransitionDirection::none;
    revealedPx_ = 0;
  }

  bool pending() const { return pending_; }
  bool active() const { return active_; }
  bool running() const { return pending_ || active_; }

 private:
  PageTransitionRegion revealTo(int16_t target) {
    if (target <= revealedPx_) return {};
    if (target > screenWidth_) target = screenWidth_;
    PageTransitionRegion region;
    region.width = target - revealedPx_;
    region.x = direction_ == PageTransitionDirection::fromRight
        ? screenWidth_ - target : revealedPx_;
    revealedPx_ = target;
    if (revealedPx_ >= screenWidth_) {
      active_ = false;
      direction_ = PageTransitionDirection::none;
    }
    return region;
  }

  PageTransitionDirection direction_ = PageTransitionDirection::none;
  bool pending_ = false;
  bool active_ = false;
  uint32_t startedAtMs_ = 0;
  int16_t screenWidth_ = 0;
  int16_t revealedPx_ = 0;
};

}  // namespace pokepod
