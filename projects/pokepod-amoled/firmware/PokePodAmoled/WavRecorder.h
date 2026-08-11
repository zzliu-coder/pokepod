#pragma once

#include <Arduino.h>
#include <FS.h>

#include "AudioFrontEnd.h"
#include "BoardConfig.h"
#include "RecorderOutcome.h"
#include "RecordingAdmissionPolicy.h"

namespace pokepod {

class WavRecorder {
 public:
  bool begin(fs::FS &fs, Print &log);
  bool start(Print &log, const String &recordingId, const String &createdAt);
  bool start(Print &log, const String &recordingId, const String &createdAt,
             const RecordingSpaceSnapshot &space);
  bool append(const uint8_t *data, size_t length, Print &log);
  bool stop(Print &log,
            RecorderStopReason reason = RecorderStopReason::user);
  bool recoverInterrupted(Print &log, const String &recoveredAt);

  bool recording() const { return recording_; }
  uint32_t durationMs() const;
  const String &finalPath() const { return finalPath_; }
  const String &capsuleId() const { return recordingId_; }
  const AudioFrontEndMetrics &audioMetrics() const {
    return audioFrontEnd_.metrics();
  }
  const RecorderOutcome &terminalResult() const {
    return terminalState_.peek();
  }
  bool takeTerminalResult(RecorderOutcome &outcome) {
    return terminalState_.take(outcome);
  }

 private:
  bool startInternal(Print &log, const String &recordingId,
                     const String &createdAt,
                     const RecordingSpaceSnapshot *space);
  void resetSessionState();
  bool finishFailure(Print &log, RecorderTerminal terminal,
                     RecorderFailureStage stage);
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
  RecorderOutcomeState terminalState_;
  AudioFrontEnd audioFrontEnd_;
};

}  // namespace pokepod
