#pragma once

#include <stdint.h>

namespace pokepod {

enum class RecorderOperationOwner : uint8_t {
  none = 0,
  localApp,
  linkUsb,
  linkWifi,
};

enum class RecorderTerminal : uint8_t {
  none = 0,
  completed,
  storageFailure,
  headerFailure,
  metadataFailure,
  commitFailure,
  tooShort,
  admissionFailure,
  captureFailure,
  cancelled,
  cleanupBlocked,
};

enum class RecorderStopReason : uint8_t {
  none = 0,
  user,
  maxDuration,
};

enum class RecorderFailureStage : uint8_t {
  none = 0,
  invalidStart,
  capacityUnknown,
  capacityInvalid,
  insufficientSpace,
  stagingExists,
  createDirectory,
  initialMetadata,
  openAudio,
  initialHeader,
  shortWrite,
  flushAudio,
  finalHeader,
  emptyAudio,
  commitAudio,
  capsuleMetadata,
  processingMetadata,
  commitDirectory,
  recoveryCheckpoint,
  storageBusy,
  captureIncomplete,
};

struct RecorderOutcome {
  RecorderTerminal terminal = RecorderTerminal::none;
  RecorderStopReason stopReason = RecorderStopReason::none;
  RecorderFailureStage failureStage = RecorderFailureStage::none;
  uint32_t dataBytes = 0;

  bool success() const { return terminal == RecorderTerminal::completed; }
  bool pending() const { return terminal != RecorderTerminal::none; }
};

class RecorderOutcomeState {
 public:
  void reset() { outcome_ = RecorderOutcome{}; }

  void complete(RecorderStopReason reason, uint32_t dataBytes) {
    outcome_.terminal = RecorderTerminal::completed;
    outcome_.stopReason = reason;
    outcome_.failureStage = RecorderFailureStage::none;
    outcome_.dataBytes = dataBytes;
  }

  void fail(RecorderTerminal terminal, RecorderFailureStage stage,
            uint32_t dataBytes) {
    if (terminal == RecorderTerminal::none ||
        terminal == RecorderTerminal::completed) {
      return;
    }
    outcome_.terminal = terminal;
    outcome_.stopReason = RecorderStopReason::none;
    outcome_.failureStage = stage;
    outcome_.dataBytes = dataBytes;
  }

  const RecorderOutcome &peek() const { return outcome_; }

  bool take(RecorderOutcome &outcome) {
    if (!outcome_.pending()) return false;
    outcome = outcome_;
    reset();
    return true;
  }

 private:
  RecorderOutcome outcome_{};
};

inline const char *recorderFailureStageName(RecorderFailureStage stage) {
  switch (stage) {
    case RecorderFailureStage::invalidStart: return "invalid_start";
    case RecorderFailureStage::capacityUnknown: return "capacity_unknown";
    case RecorderFailureStage::capacityInvalid: return "capacity_invalid";
    case RecorderFailureStage::insufficientSpace: return "insufficient_space";
    case RecorderFailureStage::stagingExists: return "staging_exists";
    case RecorderFailureStage::createDirectory: return "mkdir";
    case RecorderFailureStage::initialMetadata: return "initial_metadata";
    case RecorderFailureStage::openAudio: return "open";
    case RecorderFailureStage::initialHeader: return "initial_header";
    case RecorderFailureStage::shortWrite: return "short_write";
    case RecorderFailureStage::flushAudio: return "flush";
    case RecorderFailureStage::finalHeader: return "final_header";
    case RecorderFailureStage::emptyAudio: return "empty_audio";
    case RecorderFailureStage::commitAudio: return "audio_commit";
    case RecorderFailureStage::capsuleMetadata: return "capsule_metadata";
    case RecorderFailureStage::processingMetadata: return "processing_metadata";
    case RecorderFailureStage::commitDirectory: return "directory_commit";
    case RecorderFailureStage::recoveryCheckpoint: return "recovery_checkpoint";
    case RecorderFailureStage::storageBusy: return "storage_busy";
    case RecorderFailureStage::captureIncomplete: return "capture_incomplete";
    default: return "none";
  }
}

}  // namespace pokepod
