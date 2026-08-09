#pragma once

#include <stdint.h>

#include "VoiceSessionController.h"

namespace pokepod {

struct BleVoiceQualitySnapshot {
  uint32_t notifyAttempts = 0;
  uint32_t notifyAccepted = 0;
  uint32_t notifyFailures = 0;
  uint32_t queueOverflows = 0;
  uint32_t sessionFailures = 0;
  uint32_t readyTimeouts = 0;
  uint32_t stopAckTimeouts = 0;
  uint32_t streamTimeouts = 0;
  uint16_t lastErrorCode = 0;

  uint32_t issueCount() const {
    return notifyFailures + queueOverflows + readyTimeouts +
        stopAckTimeouts + streamTimeouts;
  }
};

class BleVoiceQualityCounters {
 public:
  void recordNotifyAttempt() { ++value_.notifyAttempts; }
  void recordNotifyStatus(bool acceptedByHost) {
    if (acceptedByHost) {
      ++value_.notifyAccepted;
    } else {
      ++value_.notifyFailures;
    }
  }
  void recordSessionError(VoiceSessionError error) {
    if (error == VoiceSessionError::none) return;
    ++value_.sessionFailures;
    value_.lastErrorCode = static_cast<uint16_t>(error);
    if (error == VoiceSessionError::queueOverflow) {
      ++value_.queueOverflows;
    } else if (error == VoiceSessionError::readyTimeout) {
      ++value_.readyTimeouts;
    } else if (error == VoiceSessionError::stopAckTimeout) {
      ++value_.stopAckTimeouts;
    } else if (error == VoiceSessionError::streamTimeout) {
      ++value_.streamTimeouts;
    }
  }
  BleVoiceQualitySnapshot snapshot() const { return value_; }

 private:
  BleVoiceQualitySnapshot value_;
};

}  // namespace pokepod
