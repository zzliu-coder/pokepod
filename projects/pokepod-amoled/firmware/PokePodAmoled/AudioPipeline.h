#pragma once

#include <Arduino.h>
#include <ESP_I2S.h>

#include "BoardConfig.h"

namespace pokepod {

class AudioPipeline {
 public:
  bool begin(Print &log);
  size_t read(uint8_t *buffer, size_t capacity);
  bool ready() const { return ready_; }
  uint64_t bytesRead() const { return bytesRead_; }
  uint32_t readFailures() const { return readFailures_; }
  uint16_t peakSample() const { return peakSample_; }

 private:
  I2SClass i2s_;
  bool ready_ = false;
  uint64_t bytesRead_ = 0;
  uint32_t readFailures_ = 0;
  uint16_t peakSample_ = 0;
};

}  // namespace pokepod
