#pragma once

#include <Arduino.h>
#include <FS.h>

#include "AudioDecimator.h"
#include "BoardConfig.h"

namespace pokepod {

class WavRecorder {
 public:
  bool begin(fs::FS &fs, Print &log);
  bool start(Print &log, const String &recordingId, const String &createdAt);
  bool append(const uint8_t *data, size_t length, Print &log);
  bool stop(Print &log);
  bool recoverInterrupted(Print &log, const String &recoveredAt);

  bool recording() const { return recording_; }
  uint32_t durationMs() const;
  const String &finalPath() const { return finalPath_; }
  const String &capsuleId() const { return recordingId_; }

 private:
  bool ensureDirectory(const char *path, Print &log);
  bool writeTextAtomically(const String &finalPath, const String &text, Print &log);
  bool writeHeader(uint32_t dataBytes);
  bool finalizePartialAudio(Print &log);
  bool writeCapsuleMetadata(Print &log, const String &timestamp);
  bool writeProcessingMetadata(Print &log, const char *status, uint32_t revision,
                               const char *errorStage, const char *error);
  bool commitStagingDirectory(Print &log);
  bool recoverStagingDirectory(const String &stagingDirectory,
                               const String &recordingId,
                               const String &recoveredAt, Print &log);

  fs::FS *fs_ = nullptr;
  File file_;
  String recordingId_;
  String createdAt_;
  String directory_;
  String partialPath_;
  String finalPath_;
  uint32_t dataBytes_ = 0;
  bool recording_ = false;
  AudioDecimator3 decimator_;
};

}  // namespace pokepod
