#pragma once

#include <stdint.h>

#include "RecorderStartState.h"

namespace pokepod {

enum class LocalRecordingStartAction : uint8_t {
  idle = 0,
  waiting,
  startCapture,
  cleanup,
};

// Main-loop bridge between storage admission and local I2S capture. Storage,
// File and terminal facts remain owned by WavRecorder.
class LocalRecordingStartState {
 public:
  bool begin(uint32_t captureSessionId) {
    if (active_ || captureSessionId == 0) return false;
    active_ = true;
    cancelRequested_ = false;
    captureSessionId_ = captureSessionId;
    return true;
  }

  void requestCancel() {
    if (active_) cancelRequested_ = true;
  }

  bool gatePermitted() const { return active_ && !cancelRequested_; }

  LocalRecordingStartAction observe(RecorderStartPollResult result,
                                    uint32_t &captureSessionId) {
    captureSessionId = 0;
    if (!active_) return LocalRecordingStartAction::idle;
    if (result == RecorderStartPollResult::pending ||
        result == RecorderStartPollResult::idle) {
      return LocalRecordingStartAction::waiting;
    }
    captureSessionId = captureSessionId_;
    const bool startCapture =
        result == RecorderStartPollResult::started && !cancelRequested_;
    reset();
    return startCapture ? LocalRecordingStartAction::startCapture
                        : LocalRecordingStartAction::cleanup;
  }

  void reset() {
    captureSessionId_ = 0;
    active_ = false;
    cancelRequested_ = false;
  }

  bool active() const { return active_; }
  bool cancelRequested() const { return cancelRequested_; }
  uint32_t captureSessionId() const { return captureSessionId_; }

 private:
  uint32_t captureSessionId_ = 0;
  bool active_ = false;
  bool cancelRequested_ = false;
};

}  // namespace pokepod
