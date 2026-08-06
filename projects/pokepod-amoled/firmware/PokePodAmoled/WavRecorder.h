#pragma once

#include <Arduino.h>
#include <FS.h>

#include "BoardConfig.h"

namespace pokepod {

class WavRecorder {
 public:
  bool start(fs::FS &fs, Print &log, const String &recordingId);
  bool append(const uint8_t *data, size_t length, Print &log);
  bool stop(Print &log);

  bool recording() const { return recording_; }
  uint32_t durationMs() const;
  const String &finalPath() const { return finalPath_; }

 private:
  bool writeHeader(uint32_t dataBytes);
  bool writeMetadata(Print &log);

  fs::FS *fs_ = nullptr;
  File file_;
  String recordingId_;
  String directory_;
  String partialPath_;
  String finalPath_;
  uint32_t dataBytes_ = 0;
  bool recording_ = false;
};

}  // namespace pokepod
