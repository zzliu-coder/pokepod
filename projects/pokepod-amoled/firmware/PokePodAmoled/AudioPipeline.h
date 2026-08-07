#pragma once

#include <Arduino.h>
#include <ESP_I2S.h>
#include <FS.h>

#include "BoardConfig.h"
#include "PeakWindow.h"

namespace pokepod {

class AudioPipeline {
 public:
  bool begin(Print &log);
  size_t read(uint8_t *buffer, size_t capacity);
  bool startPlayback(fs::FS &fs, const String &path, Print &log);
  void pumpPlayback(Print &log);
  void stopPlayback(Print &log);
  bool ready() const { return ready_; }
  bool playing() const { return playing_; }
  uint64_t bytesRead() const { return bytesRead_; }
  uint32_t readFailures() const { return readFailures_; }
  uint16_t peakSample() const { return peakWindow_.latest(); }
  uint16_t consumePeakWindow() { return peakWindow_.consume(); }
  void resetPeakWindow() { peakWindow_.reset(); }

 private:
  I2SClass i2s_;
  bool ready_ = false;
  uint64_t bytesRead_ = 0;
  uint32_t readFailures_ = 0;
  PeakWindow peakWindow_;
  File playbackFile_;
  uint32_t playbackRemaining_ = 0;
  bool playing_ = false;
  uint8_t playbackInput_[96] = {};
  uint8_t playbackOutput_[576] = {};
};

}  // namespace pokepod
