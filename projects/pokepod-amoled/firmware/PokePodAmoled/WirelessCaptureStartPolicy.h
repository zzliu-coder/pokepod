#pragma once

#include <cstdint>

#include "AudioCaptureService.h"

namespace pokepod {

enum class WirelessCaptureStartAction : uint8_t {
  waiting,
  startBleSession,
  abortCapture,
};

// The microphone becomes authoritative before BLE sessionStart.  This keeps
// the host's 400 ms ready watchdog from observing a session that cannot yet
// produce one complete PCM frame.
class WirelessCaptureStartPolicy {
 public:
  static constexpr uint32_t kFirstFrameDeadlineMs = 500;

  bool begin(uint32_t sessionId, uint32_t nowMs) {
    if (active_ || sessionId == 0) return false;
    active_ = true;
    sessionId_ = sessionId;
    startedAtMs_ = nowMs;
    return true;
  }

  WirelessCaptureStartAction observe(
      uint32_t nowMs, const AudioCaptureServiceMetrics &metrics) const {
    if (!active_) return WirelessCaptureStartAction::waiting;
    if (metrics.ring.sessionId != sessionId_ ||
        metrics.firstFailure != AudioCaptureFailureCode::none) {
      return WirelessCaptureStartAction::abortCapture;
    }
    if (metrics.ring.currentFrames != 0 && metrics.ring.pushedFrames != 0) {
      return WirelessCaptureStartAction::startBleSession;
    }
    if (static_cast<int32_t>(nowMs - startedAtMs_) >=
        static_cast<int32_t>(kFirstFrameDeadlineMs)) {
      return WirelessCaptureStartAction::abortCapture;
    }
    return WirelessCaptureStartAction::waiting;
  }

  void finish() {
    active_ = false;
    sessionId_ = 0;
    startedAtMs_ = 0;
  }

  bool active() const { return active_; }
  uint32_t sessionId() const { return sessionId_; }

 private:
  bool active_ = false;
  uint32_t sessionId_ = 0;
  uint32_t startedAtMs_ = 0;
};

}  // namespace pokepod
