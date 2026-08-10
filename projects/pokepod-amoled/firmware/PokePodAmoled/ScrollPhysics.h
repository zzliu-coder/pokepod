#pragma once

#include <stdint.h>

namespace pokepod {

class ScrollPhysics {
 public:
  static constexpr int32_t kMinimumCoastVelocity = 90;
  static constexpr int32_t kMaximumVelocity = 1800;
  static constexpr int32_t kDeceleration = 2200;
  static constexpr uint32_t kStoppedFingerMs = 90;

  int32_t positionPx() const { return positionQ8_ >> 8; }
  int32_t maximumPx() const { return maximumPx_; }
  int32_t velocityPxPerSecond() const { return velocityPxPerSecond_; }
  bool dragging() const { return dragging_; }
  bool coasting() const { return coasting_; }
  bool active() const { return dragging_ || coasting_; }

  bool setMaximum(int32_t maximumPx) {
    maximumPx_ = maximumPx > 0 ? maximumPx : 0;
    const int32_t before = positionQ8_;
    clampPosition();
    if (maximumPx_ == 0) cancelMotion();
    return before != positionQ8_;
  }

  void reset() {
    positionQ8_ = 0;
    maximumPx_ = 0;
    cancelMotion();
  }

  bool beginDrag(int16_t y, uint32_t nowMs) {
    if (maximumPx_ <= 0) return false;
    dragging_ = true;
    coasting_ = false;
    velocityPxPerSecond_ = 0;
    lastY_ = y;
    lastSampleMs_ = nowMs;
    lastMovementMs_ = nowMs;
    return true;
  }

  bool dragTo(int16_t y, uint32_t nowMs) {
    if (!dragging_) return false;
    const int32_t delta = static_cast<int32_t>(lastY_) - y;
    uint32_t elapsed = static_cast<uint32_t>(nowMs - lastSampleMs_);
    if (elapsed == 0) elapsed = 1;
    lastY_ = y;
    lastSampleMs_ = nowMs;
    if (delta == 0) return false;
    lastMovementMs_ = nowMs;
    const int32_t before = positionQ8_;
    positionQ8_ += delta * 256;
    clampPosition();
    int32_t sampleVelocity = delta * 1000 / static_cast<int32_t>(elapsed);
    sampleVelocity = clampVelocity(sampleVelocity);
    velocityPxPerSecond_ =
        (velocityPxPerSecond_ + sampleVelocity * 3) / 4;
    if (positionQ8_ == 0 || positionQ8_ == maximumPx_ * 256) {
      if ((positionQ8_ == 0 && velocityPxPerSecond_ < 0) ||
          (positionQ8_ != 0 && velocityPxPerSecond_ > 0)) {
        velocityPxPerSecond_ = 0;
      }
    }
    return before != positionQ8_;
  }

  void endDrag(uint32_t nowMs) {
    if (!dragging_) return;
    dragging_ = false;
    if (static_cast<uint32_t>(nowMs - lastMovementMs_) > kStoppedFingerMs ||
        absolute(velocityPxPerSecond_) < kMinimumCoastVelocity) {
      velocityPxPerSecond_ = 0;
      coasting_ = false;
      return;
    }
    coasting_ = true;
    lastSampleMs_ = nowMs;
  }

  bool tick(uint32_t nowMs) {
    if (!coasting_) return false;
    const uint32_t elapsed = static_cast<uint32_t>(nowMs - lastSampleMs_);
    if (elapsed == 0) return false;
    lastSampleMs_ = nowMs;
    const int32_t before = positionQ8_;

    const int32_t direction = velocityPxPerSecond_ < 0 ? -1 : 1;
    const int32_t speed = absolute(velocityPxPerSecond_);
    const uint32_t stopMs = static_cast<uint32_t>(
        (static_cast<int64_t>(speed) * 1000 + kDeceleration - 1) /
        kDeceleration);
    const uint32_t motionMs = elapsed < stopMs ? elapsed : stopMs;
    int64_t distanceQ8 = static_cast<int64_t>(speed) * motionMs * 256 / 1000;
    distanceQ8 -= static_cast<int64_t>(kDeceleration) * motionMs *
        motionMs * 256 / 2000000;
    positionQ8_ += static_cast<int32_t>(direction * distanceQ8);
    clampPosition();

    int32_t remainingSpeed = speed - static_cast<int32_t>(
        static_cast<int64_t>(kDeceleration) * elapsed / 1000);
    if (remainingSpeed < 0) remainingSpeed = 0;
    velocityPxPerSecond_ = direction * remainingSpeed;
    if (positionQ8_ == 0 || positionQ8_ == maximumPx_ * 256 ||
        absolute(velocityPxPerSecond_) < kMinimumCoastVelocity) {
      velocityPxPerSecond_ = 0;
      coasting_ = false;
    }
    return before != positionQ8_;
  }

  void cancelMotion() {
    dragging_ = false;
    coasting_ = false;
    velocityPxPerSecond_ = 0;
  }

 private:
  static int32_t absolute(int32_t value) {
    return value < 0 ? -value : value;
  }

  static int32_t clampVelocity(int32_t velocity) {
    if (velocity > kMaximumVelocity) return kMaximumVelocity;
    if (velocity < -kMaximumVelocity) return -kMaximumVelocity;
    return velocity;
  }

  void clampPosition() {
    if (positionQ8_ < 0) positionQ8_ = 0;
    const int32_t maximumQ8 = maximumPx_ * 256;
    if (positionQ8_ > maximumQ8) positionQ8_ = maximumQ8;
  }

  int32_t positionQ8_ = 0;
  int32_t maximumPx_ = 0;
  int32_t velocityPxPerSecond_ = 0;
  int16_t lastY_ = 0;
  uint32_t lastSampleMs_ = 0;
  uint32_t lastMovementMs_ = 0;
  bool dragging_ = false;
  bool coasting_ = false;
};

}  // namespace pokepod
