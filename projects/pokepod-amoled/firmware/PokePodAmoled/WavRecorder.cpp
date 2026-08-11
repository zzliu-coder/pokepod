#include "WavRecorder.h"

#include "CapsulePolicy.h"
#include "WavFormat.h"

#include <unistd.h>

namespace pokepod {
namespace {

String processingJson(const String &id, uint32_t durationMs, const char *status,
                      uint32_t revision, const char *errorStage,
                      const char *error) {
  String value;
  value.reserve(640);
  value += "{\n  \"schemaVersion\": 2,\n  \"capsuleId\": \"";
  value += id;
  value += "\",\n  \"revision\": ";
  value += revision;
  value += ",\n  \"durationMs\": ";
  value += durationMs;
  value += ",\n  \"status\": \"";
  value += status;
  value += "\",\n  \"audioFile\": \"";
  value += kCapsuleWavFile;
  value += "\",\n  \"audioFormat\": \"";
  value += kCapsuleWavFormat;
  value += "\",\n";
  value += "  \"sampleRateHz\": 16000,\n  \"channels\": 1,\n";
  value += "  \"bitsPerSample\": 16,\n  \"rawTextFile\": null,\n";
  value += "  \"polishedTextFile\": null,\n  \"errorStage\": ";
  if (errorStage == nullptr) value += "null";
  else {
    value += '"';
    value += errorStage;
    value += '"';
  }
  value += ",\n  \"error\": ";
  if (error == nullptr) value += "null";
  else {
    value += '"';
    value += error;
    value += '"';
  }
  value += ",\n  \"attempts\": 0,\n  \"engine\": \"tencent-asr\",\n";
  value += "  \"model\": \"16k_zh\"\n}\n";
  return value;
}

String capsuleJson(const String &id, const String &timestamp) {
  String value;
  value.reserve(512);
  value += "{\n  \"schemaVersion\": 1,\n  \"id\": \"";
  value += id;
  value += "\",\n  \"title\": \"语音胶囊\",\n  \"createdAt\": \"";
  value += timestamp;
  value += "\",\n  \"updatedAt\": \"";
  value += timestamp;
  value += "\",\n  \"revision\": 1,\n  \"favorite\": false,\n";
  value += "  \"tags\": [],\n  \"language\": \"zh\",\n  \"contentHash\": null\n}\n";
  return value;
}

}  // namespace

bool WavRecorder::begin(fs::FS &fs, Print &log) {
  fs_ = &fs;
  if (!transaction_.begin(fs, log) ||
      !transaction_.recoverAll(StorageOwner::recovery)) return false;
  StorageReservation reservation = StorageCoordinator::instance().reserve(
      StorageOwner::recovery, StorageAccess::mutation, 1000);
  return reservation && ensureDirectory(kCapsuleRoot, log) &&
         ensureDirectory(kCapsuleInbox, log) &&
         ensureDirectory(kCapsuleArchive, log) &&
         ensureDirectory(kCapsuleStaging, log) &&
         ensureDirectory(kCapsuleTrash, log) &&
         ensureDirectory(kCapsuleSystem, log);
}

bool WavRecorder::start(Print &log, const String &recordingId,
                        const String &createdAt) {
  // The integration layer must provide a fresh SD capacity snapshot.  Keep
  // this overload so older callers still compile, but fail closed until they
  // adopt the admission-aware API.
  const RecordingSpaceSnapshot unknown{};
  return startInternal(log, recordingId, createdAt, &unknown);
}

bool WavRecorder::start(Print &log, const String &recordingId,
                        const String &createdAt,
                        const RecordingSpaceSnapshot &space) {
  return startInternal(log, recordingId, createdAt, &space);
}

bool WavRecorder::startInternal(Print &log, const String &recordingId,
                                const String &createdAt,
                                const RecordingSpaceSnapshot *space) {
  if (recording_) {
    log.println("{\"event\":\"recording_error\",\"stage\":\"invalid_start\"}");
    return false;
  }
  resetSessionState();
  if (fs_ == nullptr || !isUuid(recordingId.c_str()) || createdAt.isEmpty()) {
    return finishFailure(log, RecorderTerminal::admissionFailure,
                         RecorderFailureStage::invalidStart);
  }

  const RecordingAdmission admission =
      evaluateRecordingAdmission(space == nullptr
                                     ? RecordingSpaceSnapshot{}
                                     : *space);
  if (!admission.allowed()) {
    RecorderFailureStage stage = RecorderFailureStage::insufficientSpace;
    if (admission.reason == RecordingAdmissionReason::capacityUnknown) {
      stage = RecorderFailureStage::capacityUnknown;
    } else if (admission.reason ==
               RecordingAdmissionReason::invalidCapacity) {
      stage = RecorderFailureStage::capacityInvalid;
    }
    log.printf("{\"event\":\"recording_admission_rejected\",\"stage\":\"%s\",\"available_bytes\":%llu,\"required_bytes\":%llu}\n",
               recorderFailureStageName(stage),
               static_cast<unsigned long long>(admission.availableBytes),
               static_cast<unsigned long long>(admission.requiredBytes));
    return finishFailure(log, RecorderTerminal::admissionFailure, stage);
  }

  storageReservation_ = StorageCoordinator::instance().reserve(
      StorageOwner::recorder, StorageAccess::mutation, 250);
  if (!storageReservation_) {
    return finishFailure(log, RecorderTerminal::storageFailure,
                         RecorderFailureStage::storageBusy);
  }

  recordingId_ = recordingId;
  createdAt_ = createdAt;
  directory_ = String(kCapsuleStaging) + "/" + recordingId_;
  partialPath_ = directory_ + "/audio.wav.part";
  finalPath_ = directory_ + "/audio.wav";

  StorageIoLease startIo = StorageCoordinator::instance().acquireIo(
      StorageOwner::recorder, StorageAccess::mutation, 1000);
  if (!startIo) {
    return finishFailure(log, RecorderTerminal::storageFailure,
                         RecorderFailureStage::storageBusy);
  }
  if (fs_->exists(directory_)) {
    return finishFailure(log, RecorderTerminal::commitFailure,
                         RecorderFailureStage::stagingExists);
  }
  if (!fs_->mkdir(directory_)) {
    return finishFailure(log, RecorderTerminal::storageFailure,
                         RecorderFailureStage::createDirectory);
  }
  startIo.release();
  checkpointInitialized_ = initializeRecorderCheckpoint(
      checkpoint_, recordingId_.c_str(), createdAt_.c_str());
  if (!checkpointInitialized_) {
    return finishFailure(log, RecorderTerminal::metadataFailure,
                         RecorderFailureStage::recoveryCheckpoint);
  }
  if (!writeProcessingMetadata(log, "recording", 1, nullptr, nullptr)) {
    return finishFailure(log, RecorderTerminal::metadataFailure,
                         RecorderFailureStage::initialMetadata);
  }

  StorageIoLease openIo = StorageCoordinator::instance().acquireIo(
      StorageOwner::recorder, StorageAccess::mutation, 1000);
  if (!openIo) {
    return finishFailure(log, RecorderTerminal::storageFailure,
                         RecorderFailureStage::storageBusy);
  }
  file_ = fs_->open(partialPath_, FILE_WRITE);
  if (!file_) {
    return finishFailure(log, RecorderTerminal::storageFailure,
                         RecorderFailureStage::openAudio);
  }
  if (!writeHeader(0)) {
    file_.close();
    fs_->remove(partialPath_);
    return finishFailure(log, RecorderTerminal::headerFailure,
                         RecorderFailureStage::initialHeader);
  }
  openIo.release();
  if (!persistCheckpoint(false, RecorderFailureStage::none, log)) {
    return finishFailure(log, RecorderTerminal::metadataFailure,
                         RecorderFailureStage::recoveryCheckpoint);
  }
  recording_ = true;
  log.printf("{\"event\":\"recording_started\",\"id\":\"%s\",\"format\":\"16k_s16le_mono\"}\n",
             recordingId_.c_str());
  return true;
}

bool WavRecorder::append(const uint8_t *data, size_t length, Print &log) {
  if (!recording_ || !file_ || data == nullptr) return false;
  uint8_t mono[kAudioBytesPerChunk];
  size_t offset = 0;
  while (offset + 4 <= length) {
    size_t inputBytes = length - offset;
    if (inputBytes > kAudioBytesPerChunk) inputBytes = kAudioBytesPerChunk;
    inputBytes -= inputBytes % 4;
    const size_t converted = audioFrontEnd_.processStereo16(
        data + offset, inputBytes, mono, sizeof(mono));
    if (converted > 0 && !appendMonoBytes(mono, converted, log)) return false;
    if (durationMs() >= kMaxRecordingMs) {
      return stop(log, RecorderStopReason::maxDuration);
    }
    offset += inputBytes;
  }
  if (durationMs() >= kMaxRecordingMs) {
    return stop(log, RecorderStopReason::maxDuration);
  }
  return true;
}

bool WavRecorder::appendMono16(const int16_t *samples, size_t sampleCount,
                               Print &log) {
  if (samples == nullptr || sampleCount == 0 ||
      sampleCount > SIZE_MAX / sizeof(int16_t)) {
    return false;
  }
  if (!appendMonoBytes(reinterpret_cast<const uint8_t *>(samples),
                       sampleCount * sizeof(int16_t), log)) {
    return false;
  }
  if (durationMs() >= kMaxRecordingMs) {
    return stop(log, RecorderStopReason::maxDuration);
  }
  return true;
}

bool WavRecorder::appendMonoBytes(const uint8_t *data, size_t length,
                                  Print &log) {
  if (!recording_ || !file_ || data == nullptr || length == 0 ||
      (length & 1U) != 0) {
    return false;
  }
  if (dataBytes_ >= kMaximumRecordingAudioBytes) return false;
  const size_t remaining = static_cast<size_t>(
      kMaximumRecordingAudioBytes - dataBytes_);
  if (length > remaining) length = remaining & ~static_cast<size_t>(1U);
  if (length == 0) return false;
  StorageIoLease writeIo = StorageCoordinator::instance().acquireIo(
      StorageOwner::recorder, StorageAccess::mutation, 1000);
  if (!writeIo) {
    return finishFailure(log, RecorderTerminal::storageFailure,
                         RecorderFailureStage::storageBusy);
  }
  const size_t written = file_.write(data, length);
  dataBytes_ += static_cast<uint32_t>(written);
  checkpointCrcState_ = recorderAudioCrc32Update(
      checkpointCrcState_, data, written);
  if (written != length) {
    log.printf("{\"event\":\"recording_error\",\"stage\":\"short_write\",\"expected\":%u,\"actual\":%u}\n",
               static_cast<unsigned>(length), static_cast<unsigned>(written));
    return finishFailure(log, RecorderTerminal::storageFailure,
                         RecorderFailureStage::shortWrite);
  }
  writeIo.release();
  if (recorderCheckpointDue(checkpointedBytes_, dataBytes_) &&
      !persistCheckpoint(false, RecorderFailureStage::none, log)) {
    return finishFailure(log, RecorderTerminal::metadataFailure,
                         RecorderFailureStage::recoveryCheckpoint);
  }
  return true;
}

bool WavRecorder::stop(Print &log, RecorderStopReason reason) {
  if (!recording_) return false;
  recording_ = false;
  StorageIoLease stopIo = StorageCoordinator::instance().acquireIo(
      StorageOwner::recorder, StorageAccess::mutation, 1000);
  if (!stopIo) {
    return finishFailure(log, RecorderTerminal::storageFailure,
                         RecorderFailureStage::storageBusy);
  }
  file_.flush();
  if (file_.getWriteError() != 0) {
    file_.close();
    return finishFailure(log, RecorderTerminal::storageFailure,
                         RecorderFailureStage::flushAudio);
  }
  const bool headerOk = file_.seek(0) && writeHeader(dataBytes_);
  file_.flush();
  const bool finalFlushOk = file_.getWriteError() == 0;
  file_.close();
  stopIo.release();
  if (!headerOk) {
    return finishFailure(log, RecorderTerminal::headerFailure,
                         RecorderFailureStage::finalHeader);
  }
  if (!finalFlushOk) {
    return finishFailure(log, RecorderTerminal::storageFailure,
                         RecorderFailureStage::flushAudio);
  }
  if (dataBytes_ == 0) {
    return finishFailure(log, RecorderTerminal::tooShort,
                         RecorderFailureStage::emptyAudio);
  }
  if (!finalizePartialAudio(log)) {
    return finishFailure(log, RecorderTerminal::commitFailure,
                         RecorderFailureStage::commitAudio);
  }
  if (!writeCapsuleMetadata(log, createdAt_)) {
    return finishFailure(log, RecorderTerminal::metadataFailure,
                         RecorderFailureStage::capsuleMetadata);
  }
  if (!writeProcessingMetadata(log, "queued", 2, nullptr, nullptr)) {
    return finishFailure(log, RecorderTerminal::metadataFailure,
                         RecorderFailureStage::processingMetadata);
  }
  if (!commitStagingDirectory(log)) {
    return finishFailure(log, RecorderTerminal::commitFailure,
                         RecorderFailureStage::commitDirectory);
  }
  if (!removeCheckpoint()) {
    log.println("{\"event\":\"recording_warning\",\"stage\":\"checkpoint_cleanup\"}");
  }
  terminalState_.complete(reason, dataBytes_);
  const AudioFrontEndMetrics &audio = audioFrontEnd_.metrics();
  log.printf("{\"event\":\"recording_stopped\",\"ok\":true,\"duration_ms\":%lu,\"bytes\":%lu,\"path\":\"%s\",\"audio_channel\":\"%s\",\"left_peak\":%u,\"right_peak\":%u,\"output_peak\":%u,\"noise_floor\":%u,\"suppressed_samples\":%lu,\"limited_samples\":%lu,\"maximum_gain_q12\":%lu}\n",
             static_cast<unsigned long>(durationMs()),
             static_cast<unsigned long>(dataBytes_), finalPath_.c_str(),
             audioInputChannelName(audio.selectedChannel), audio.leftPeak,
             audio.rightPeak, audio.outputPeak, audio.estimatedNoiseFloor,
             static_cast<unsigned long>(audio.suppressedSamples),
             static_cast<unsigned long>(audio.limitedSamples),
             static_cast<unsigned long>(audio.maximumGainQ12));
  storageReservation_.release();
  return true;
}

uint32_t WavRecorder::durationMs() const {
  return audioDurationMs(dataBytes_);
}

bool WavRecorder::abortCapture(Print &log) {
  if (!recording_) return false;
  return finishFailure(log, RecorderTerminal::captureFailure,
                       RecorderFailureStage::captureIncomplete);
}

void WavRecorder::resetSessionState() {
  if (file_) file_.close();
  recording_ = false;
  recordingId_ = "";
  createdAt_ = "";
  directory_ = "";
  partialPath_ = "";
  finalPath_ = "";
  dataBytes_ = 0;
  checkpointInitialized_ = false;
  checkpointCrcState_ = recorderAudioCrc32Begin();
  checkpointedBytes_ = 0;
  memset(&checkpoint_, 0, sizeof(checkpoint_));
  storageReservation_.release();
  terminalState_.reset();
  audioFrontEnd_.reset();
}

bool WavRecorder::finishFailure(Print &log, RecorderTerminal terminal,
                                RecorderFailureStage stage) {
  recording_ = false;
  if (file_) {
    StorageIoLease lease = StorageCoordinator::instance().acquireIo(
        activeStorageOwner(), StorageAccess::mutation, 1000);
    file_.flush();
    file_.close();
  }
  if (checkpointInitialized_ && stage != RecorderFailureStage::recoveryCheckpoint) {
    persistCheckpoint(true, stage, log);
  }
  terminalState_.fail(terminal, stage, dataBytes_);
  log.printf("{\"event\":\"recording_terminal\",\"ok\":false,\"stage\":\"%s\",\"bytes\":%lu}\n",
             recorderFailureStageName(stage),
             static_cast<unsigned long>(dataBytes_));
  storageReservation_.release();
  return false;
}

bool WavRecorder::ensureDirectory(const char *path, Print &log) {
  StorageIoLease lease = StorageCoordinator::instance().acquireIo(
      activeStorageOwner(), StorageAccess::mutation, 1000);
  if (!lease) return false;
  if (fs_->exists(path) || fs_->mkdir(path)) return true;
  log.printf("{\"event\":\"storage_error\",\"stage\":\"mkdir\",\"path\":\"%s\"}\n", path);
  return false;
}

bool WavRecorder::writeTextAtomically(const String &finalPath,
                                      const String &text, Print &log) {
  const bool ok = transaction_.writeTextAtomic(
      finalPath, text, activeStorageOwner(), "recorder-metadata");
  if (!ok) {
    log.println("{\"event\":\"storage_error\",\"stage\":\"transaction_write\"}");
  }
  return ok;
}

bool WavRecorder::writeHeader(uint32_t dataBytes) {
  if (!file_) return false;
  uint8_t header[kWavHeaderBytes];
  encodeWavHeader(header, dataBytes);
  return file_.write(header, sizeof(header)) == sizeof(header) &&
      file_.getWriteError() == 0;
}

bool WavRecorder::finalizePartialAudio(Print &log) {
  if (dataBytes_ == 0) {
    log.println("{\"event\":\"recording_error\",\"stage\":\"empty_audio\"}");
    return false;
  }
  if (!transaction_.commitPreparedFile(
          recordingId_.c_str(), partialPath_, finalPath_,
          activeStorageOwner())) {
    log.println("{\"event\":\"recording_error\",\"stage\":\"audio_commit\"}");
    return false;
  }
  return true;
}

bool WavRecorder::writeCapsuleMetadata(Print &log, const String &timestamp) {
  return writeTextAtomically(directory_ + "/capsule.json",
                             capsuleJson(recordingId_, timestamp), log);
}

bool WavRecorder::writeProcessingMetadata(Print &log, const char *status,
                                          uint32_t revision,
                                          const char *errorStage,
                                          const char *error) {
  return writeTextAtomically(directory_ + "/processing.json",
                             processingJson(recordingId_, durationMs(), status,
                                            revision, errorStage, error), log);
}

bool WavRecorder::commitStagingDirectory(Print &log) {
  const String target = String(kCapsuleInbox) + "/" + recordingId_;
  StorageIoLease lease = StorageCoordinator::instance().acquireIo(
      activeStorageOwner(), StorageAccess::mutation, 1000);
  if (!lease) return false;
  if (fs_->exists(target)) {
    log.println("{\"event\":\"recording_error\",\"stage\":\"target_exists\"}");
    return false;
  }
  if (!fs_->rename(directory_, target)) {
    log.println("{\"event\":\"recording_error\",\"stage\":\"directory_commit\"}");
    return false;
  }
  directory_ = target;
  finalPath_ = target + "/audio.wav";
  return true;
}

bool WavRecorder::recoverInterrupted(Print &log, const String &recoveredAt) {
  if (fs_ == nullptr || recoveredAt.isEmpty()) return false;
  StorageReservation reservation = StorageCoordinator::instance().reserve(
      StorageOwner::recovery, StorageAccess::mutation, 1000);
  if (!reservation) return false;
  StorageIoLease scanIo = StorageCoordinator::instance().acquireIo(
      StorageOwner::recovery, StorageAccess::read, 1000);
  if (!scanIo) return false;
  File root = fs_->open(kCapsuleStaging);
  if (!root || !root.isDirectory()) return false;
  bool recoveredAny = false;
  File entry = root.openNextFile();
  while (entry) {
    const String fullName = entry.name();
    const bool directory = entry.isDirectory();
    entry.close();
    const int slash = fullName.lastIndexOf('/');
    const String id = slash >= 0 ? fullName.substring(slash + 1) : fullName;
    if (directory && isUuid(id.c_str()) &&
        recoverStagingDirectory(String(kCapsuleStaging) + "/" + id,
                                id, recoveredAt, log)) {
      recoveredAny = true;
    }
    entry = root.openNextFile();
  }
  root.close();
  return recoveredAny;
}

bool WavRecorder::recoverStagingDirectory(const String &stagingDirectory,
                                          const String &recordingId,
                                          const String &recoveredAt,
                                          Print &log) {
  const String partial = stagingDirectory + "/audio.wav.part";
  const String audio = stagingDirectory + "/audio.wav";
  const String checkpointPath = stagingDirectory + "/recording.chk";
  StorageIoLease recoveryIo = StorageCoordinator::instance().acquireIo(
      StorageOwner::recovery, StorageAccess::mutation, 1000);
  if (!recoveryIo) return false;
  StoredRecorderCheckpoint stored{};
  bool hasCheckpoint = false;
  if (fs_->exists(checkpointPath)) {
    File checkpointFile = fs_->open(checkpointPath, FILE_READ);
    hasCheckpoint = checkpointFile && !checkpointFile.isDirectory() &&
        checkpointFile.size() == sizeof(stored) &&
        checkpointFile.read(reinterpret_cast<uint8_t *>(&stored),
                            sizeof(stored)) == sizeof(stored) &&
        validateRecorderCheckpoint(stored);
    if (checkpointFile) checkpointFile.close();
    if (!hasCheckpoint) return false;
    if (!String(stored.capsuleId).equalsIgnoreCase(recordingId)) return false;
    if (stored.state == static_cast<uint8_t>(RecorderCheckpointState::failed)) {
      log.printf("{\"event\":\"recording_recovery_preserved\",\"id\":\"%s\",\"stage\":%u}\n",
                 recordingId.c_str(), stored.failureStage);
      return false;
    }
  }
  if (!fs_->exists(audio) && fs_->exists(partial)) {
    File interrupted = fs_->open(partial, FILE_READ);
    if (!interrupted || interrupted.size() <= kWavHeaderBytes ||
        interrupted.size() - kWavHeaderBytes >
            kMaximumRecordingAudioBytes) {
      if (interrupted) interrupted.close();
      return false;
    }
    const uint32_t actualBytes = static_cast<uint32_t>(
        interrupted.size() - kWavHeaderBytes);
    uint32_t bytes = actualBytes & ~1U;
    if (hasCheckpoint) {
      uint32_t crcState = recorderAudioCrc32Begin();
      uint8_t buffer[512];
      uint32_t remaining = stored.confirmedDataBytes;
      if (!interrupted.seek(kWavHeaderBytes)) {
        interrupted.close();
        return false;
      }
      while (remaining > 0) {
        const size_t wanted = remaining < sizeof(buffer)
            ? remaining : sizeof(buffer);
        const int received = interrupted.read(buffer, wanted);
        if (received <= 0) {
          interrupted.close();
          return false;
        }
        crcState = recorderAudioCrc32Update(
            crcState, buffer, static_cast<size_t>(received));
        remaining -= static_cast<uint32_t>(received);
      }
      const RecorderRecoveryPlan plan = planRecorderRecovery(
          stored, actualBytes, recorderAudioCrc32Finish(crcState));
      if (plan.disposition !=
          RecorderRecoveryDisposition::recoverConfirmedAudio) {
        interrupted.close();
        return false;
      }
      bytes = plan.recoveredDataBytes;
      interrupted.close();
      const String mountedPath = String("/sdcard") + partial;
      if (::truncate(mountedPath.c_str(),
                     static_cast<off_t>(kWavHeaderBytes + bytes)) != 0) {
        return false;
      }
    } else {
      interrupted.close();
    }
    File patch = fs_->open(partial, "r+");
    uint8_t header[kWavHeaderBytes];
    encodeWavHeader(header, bytes);
    const bool patched = patch && patch.seek(0) &&
        patch.write(header, sizeof(header)) == sizeof(header);
    if (patch) {
      patch.flush();
      patch.close();
    }
    if (!patched || !fs_->rename(partial, audio)) return false;
  }
  File recoveredAudio = fs_->open(audio, FILE_READ);
  if (!recoveredAudio || recoveredAudio.size() <= kWavHeaderBytes ||
      recoveredAudio.size() - kWavHeaderBytes >
          kMaximumRecordingAudioBytes) {
    if (recoveredAudio) recoveredAudio.close();
    return false;
  }
  const uint32_t recoveredBytes =
      static_cast<uint32_t>(recoveredAudio.size() - kWavHeaderBytes);
  recoveredAudio.close();

  recoveryIo.release();

  recordingId_ = recordingId;
  createdAt_ = hasCheckpoint ? String(stored.createdAt) : recoveredAt;
  directory_ = stagingDirectory;
  partialPath_ = partial;
  finalPath_ = audio;
  dataBytes_ = recoveredBytes;
  if (!writeCapsuleMetadata(log, createdAt_) ||
      !writeProcessingMetadata(log, "queued", 2, nullptr, nullptr) ||
      !commitStagingDirectory(log)) {
    return false;
  }
  if (!removeCheckpoint()) {
    log.println("{\"event\":\"recording_warning\",\"stage\":\"checkpoint_cleanup\"}");
  }
  log.printf("{\"event\":\"recording_recovered\",\"id\":\"%s\",\"duration_ms\":%lu}\n",
             recordingId_.c_str(), static_cast<unsigned long>(durationMs()));
  return true;
}

StorageOwner WavRecorder::activeStorageOwner() const {
  return storageReservation_ ? storageReservation_.owner()
                             : StorageOwner::recovery;
}

bool WavRecorder::persistCheckpoint(bool failed, RecorderFailureStage stage,
                                    Print &log) {
  if (!checkpointInitialized_ || directory_.isEmpty()) return false;
  if (file_) {
    StorageIoLease lease = StorageCoordinator::instance().acquireIo(
        activeStorageOwner(), StorageAccess::mutation, 1000);
    if (!lease) return false;
    file_.flush();
    if (file_.getWriteError() != 0) return false;
  }
  updateRecorderCheckpoint(checkpoint_, dataBytes_,
                           recorderAudioCrc32Finish(checkpointCrcState_));
  if (failed) failRecorderCheckpoint(checkpoint_, stage);
  const bool ok = transaction_.writeBytesAtomic(
      directory_ + "/recording.chk",
      reinterpret_cast<const uint8_t *>(&checkpoint_), sizeof(checkpoint_),
      activeStorageOwner(), recordingId_.c_str());
  if (ok) checkpointedBytes_ = checkpoint_.confirmedDataBytes;
  else log.println("{\"event\":\"recording_error\",\"stage\":\"checkpoint_write\"}");
  return ok;
}

bool WavRecorder::removeCheckpoint() {
  StorageIoLease lease = StorageCoordinator::instance().acquireIo(
      activeStorageOwner(), StorageAccess::mutation, 1000);
  if (!lease) return false;
  const String path = directory_ + "/recording.chk";
  return !fs_->exists(path) || fs_->remove(path);
}

}  // namespace pokepod
