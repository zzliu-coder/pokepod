#pragma once

#include <Arduino.h>
#include <ESP_I2S.h>
#include <FS.h>

#include "BoardConfig.h"
#include "AudioCaptureTiming.h"
#include "AudioBoardProfile.h"
#include "PeakWindow.h"
#include "PlaybackBufferPolicy.h"
#include "DeferredFileCleanup.h"
#include "StorageCoordinator.h"

namespace pokepod {

class AudioPipeline {
 public:
  bool begin(BoardVariant variant, Print &log);
  bool startCapture(Print &log);
  void stopHardware(Print &log);
  size_t read(uint8_t *buffer, size_t capacity);
  // Capture-task entry. It performs one bounded I2S read without touching UI
  // meters, logging or cross-thread diagnostic counters.
  size_t readCaptureRealtime(uint8_t *buffer, size_t capacity);
  void observeCapturedMono(const int16_t *samples, size_t count);
  bool startPlayback(fs::FS &fs, const String &path, Print &log);
  void pumpPlayback(Print &log);
  void stopPlayback(Print &log);
  bool pollPlaybackCleanup(Print &log);
  bool playbackCleanupPending() const { return playbackCleanup_.pending(); }
  bool ready() const { return available_; }
  bool active() const { return hardwareActive_; }
  bool playing() const { return playing_; }
  uint64_t bytesRead() const { return bytesRead_; }
  uint32_t readFailures() const { return readFailures_; }
  uint16_t peakSample() const { return peakWindow_.latest(); }
  uint16_t consumePeakWindow() { return peakWindow_.consume(); }
  void copyEnvelope(uint16_t *output, size_t count) const {
    peakWindow_.copyEnvelope(output, count);
  }
  void resetPeakWindow() { peakWindow_.reset(); }
  const char *lastPlaybackError() const { return lastPlaybackError_; }
  const char *lastHardwareError() const { return lastHardwareError_; }
  uint32_t captureHeapFreeBeforeStart() const {
    return captureHeapFreeBeforeStart_;
  }
  uint32_t captureHeapLargestBeforeStart() const {
    return captureHeapLargestBeforeStart_;
  }
  uint32_t playbackStartFailures() const { return playbackStartFailures_; }
  uint32_t playbackHeapLargestBeforeStart() const {
    return playbackHeapLargestBeforeStart_;
  }
  uint32_t playbackFileReadCount() const { return playbackFileReadCount_; }
  uint32_t playbackPumpCount() const { return playbackPumpCount_; }
  uint32_t playbackMaxFileReadUs() const { return playbackMaxFileReadUs_; }

 private:
  enum class HardwareMode : uint8_t { none, capture, playback };

  bool startHardware(HardwareMode mode, uint32_t sampleRate, Print &log);

  I2SClass i2s_;
  void *codec_ = nullptr;
  bool available_ = false;
  bool hardwareActive_ = false;
  HardwareMode hardwareMode_ = HardwareMode::none;
  AudioBoardProfile boardProfile_;
  uint32_t hardwareSampleRate_ = 0;
  const char *lastHardwareError_ = "none";
  uint32_t captureHeapFreeBeforeStart_ = 0;
  uint32_t captureHeapLargestBeforeStart_ = 0;
  uint64_t bytesRead_ = 0;
  uint32_t readFailures_ = 0;
  PeakWindow peakWindow_;
  File playbackFile_;
  StorageReservation playbackReservation_;
  DeferredFileCleanup playbackCleanup_;
  bool playbackCleanupLogPending_ = false;
  uint32_t playbackFileRemaining_ = 0;
  size_t playbackBufferedBytes_ = 0;
  size_t playbackBufferOffset_ = 0;
  bool playing_ = false;
  const char *lastPlaybackError_ = "none";
  uint32_t playbackStartFailures_ = 0;
  uint32_t playbackHeapLargestBeforeStart_ = 0;
  uint32_t playbackFileReadCount_ = 0;
  uint32_t playbackPumpCount_ = 0;
  uint32_t playbackMaxFileReadUs_ = 0;
  uint8_t playbackInput_[kPlaybackReadAheadBytes] = {};
  uint8_t playbackOutput_[kPlaybackFeedBytes * 2] = {};
};

}  // namespace pokepod
