#pragma once

#include <atomic>
#include <stdint.h>

namespace pokepod {

enum class AudioCaptureSessionPhase : uint8_t {
  idle,
  capturing,
  stopRequested,
  finalizePending,
};

// Cross-task ownership truth for the single microphone session. The realtime
// task only observes capturing/stopRequested and reports taskStopped(); the
// Arduino loop alone owns begin/finalize and all hardware/service teardown.
class AudioCaptureSessionState {
 public:
  bool begin() {
    AudioCaptureSessionPhase expected = AudioCaptureSessionPhase::idle;
    const bool started = phase_.compare_exchange_strong(
        expected, AudioCaptureSessionPhase::capturing,
        std::memory_order_acq_rel, std::memory_order_acquire);
    if (started) taskStopAcknowledged_.store(false, std::memory_order_release);
    return started;
  }

  bool requestStop() {
    AudioCaptureSessionPhase expected = AudioCaptureSessionPhase::capturing;
    if (phase_.compare_exchange_strong(
            expected, AudioCaptureSessionPhase::stopRequested,
            std::memory_order_acq_rel, std::memory_order_acquire)) {
      return true;
    }
    return expected == AudioCaptureSessionPhase::stopRequested ||
           expected == AudioCaptureSessionPhase::finalizePending;
  }

  bool taskStopped() {
    AudioCaptureSessionPhase expected =
        AudioCaptureSessionPhase::stopRequested;
    return phase_.compare_exchange_strong(
        expected, AudioCaptureSessionPhase::finalizePending,
        std::memory_order_acq_rel, std::memory_order_acquire);
  }

  bool acknowledgeTaskStopped() {
    if (!finalizePending()) return false;
    taskStopAcknowledged_.store(true, std::memory_order_release);
    return true;
  }

  bool finishFinalize() {
    if (!taskStopAcknowledged_.load(std::memory_order_acquire)) return false;
    AudioCaptureSessionPhase expected =
        AudioCaptureSessionPhase::finalizePending;
    const bool finished = phase_.compare_exchange_strong(
        expected, AudioCaptureSessionPhase::idle,
        std::memory_order_acq_rel, std::memory_order_acquire);
    if (finished) {
      taskStopAcknowledged_.store(false, std::memory_order_release);
    }
    return finished;
  }

  AudioCaptureSessionPhase phase() const {
    return phase_.load(std::memory_order_acquire);
  }
  bool captureActive() const {
    return phase() == AudioCaptureSessionPhase::capturing;
  }
  bool stopRequested() const {
    return phase() == AudioCaptureSessionPhase::stopRequested;
  }
  bool finalizePending() const {
    return phase() == AudioCaptureSessionPhase::finalizePending;
  }
  bool busy() const { return phase() != AudioCaptureSessionPhase::idle; }

 private:
  std::atomic<AudioCaptureSessionPhase> phase_{
      AudioCaptureSessionPhase::idle};
  std::atomic<bool> taskStopAcknowledged_{false};
};

}  // namespace pokepod
