#include "WavRecorder.h"

#include "CapsulePolicy.h"
#include "WavFormat.h"

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
  return ensureDirectory(kCapsuleRoot, log) &&
         ensureDirectory(kCapsuleInbox, log) &&
         ensureDirectory(kCapsuleArchive, log) &&
         ensureDirectory(kCapsuleStaging, log) &&
         ensureDirectory(kCapsuleTrash, log) &&
         ensureDirectory(kCapsuleSystem, log);
}

bool WavRecorder::start(Print &log, const String &recordingId,
                        const String &createdAt) {
  if (recording_ || fs_ == nullptr || !isUuid(recordingId.c_str()) ||
      createdAt.isEmpty()) {
    log.println("{\"event\":\"recording_error\",\"stage\":\"invalid_start\"}");
    return false;
  }
  recordingId_ = recordingId;
  createdAt_ = createdAt;
  directory_ = String(kCapsuleStaging) + "/" + recordingId_;
  partialPath_ = directory_ + "/audio.wav.part";
  finalPath_ = directory_ + "/audio.wav";

  if (fs_->exists(directory_)) {
    log.println("{\"event\":\"recording_error\",\"stage\":\"staging_exists\"}");
    return false;
  }
  if (!fs_->mkdir(directory_)) {
    log.println("{\"event\":\"recording_error\",\"stage\":\"mkdir\"}");
    return false;
  }
  if (!writeProcessingMetadata(log, "recording", 1, nullptr, nullptr)) return false;

  file_ = fs_->open(partialPath_, FILE_WRITE);
  if (!file_) {
    log.println("{\"event\":\"recording_error\",\"stage\":\"open\"}");
    return false;
  }
  dataBytes_ = 0;
  decimator_.reset();
  if (!writeHeader(0)) {
    file_.close();
    fs_->remove(partialPath_);
    return false;
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
    const size_t converted = decimator_.processStereo16(
        data + offset, inputBytes, mono, sizeof(mono));
    if (converted > 0) {
      const size_t written = file_.write(mono, converted);
      dataBytes_ += static_cast<uint32_t>(written);
      if (written != converted) {
        log.printf("{\"event\":\"recording_error\",\"stage\":\"short_write\",\"expected\":%u,\"actual\":%u}\n",
                   static_cast<unsigned>(converted), static_cast<unsigned>(written));
        stop(log);
        return false;
      }
    }
    offset += inputBytes;
  }
  if (durationMs() >= kMaxRecordingMs) return stop(log);
  return true;
}

bool WavRecorder::stop(Print &log) {
  if (!recording_) return false;
  recording_ = false;
  file_.flush();
  bool ok = file_.seek(0) && writeHeader(dataBytes_);
  file_.flush();
  file_.close();
  if (ok) ok = finalizePartialAudio(log);
  if (ok) ok = writeCapsuleMetadata(log, createdAt_);
  if (ok) ok = writeProcessingMetadata(log, "queued", 2, nullptr, nullptr);
  if (ok) ok = commitStagingDirectory(log);
  log.printf("{\"event\":\"recording_stopped\",\"ok\":%s,\"duration_ms\":%lu,\"bytes\":%lu,\"path\":\"%s\"}\n",
             ok ? "true" : "false", static_cast<unsigned long>(durationMs()),
             static_cast<unsigned long>(dataBytes_), finalPath_.c_str());
  return ok;
}

uint32_t WavRecorder::durationMs() const {
  return audioDurationMs(dataBytes_);
}

bool WavRecorder::ensureDirectory(const char *path, Print &log) {
  if (fs_->exists(path) || fs_->mkdir(path)) return true;
  log.printf("{\"event\":\"storage_error\",\"stage\":\"mkdir\",\"path\":\"%s\"}\n", path);
  return false;
}

bool WavRecorder::writeTextAtomically(const String &finalPath,
                                      const String &text, Print &log) {
  const String partial = finalPath + ".tmp";
  if (fs_->exists(partial) && !fs_->remove(partial)) return false;
  File output = fs_->open(partial, FILE_WRITE);
  if (!output) return false;
  const size_t bytes = output.print(text);
  const bool wrote = bytes == text.length() && output.getWriteError() == 0;
  output.flush();
  output.close();
  if (!wrote) {
    fs_->remove(partial);
    log.println("{\"event\":\"storage_error\",\"stage\":\"atomic_write\"}");
    return false;
  }
  if (fs_->exists(finalPath) && !fs_->remove(finalPath)) return false;
  if (!fs_->rename(partial, finalPath)) {
    log.println("{\"event\":\"storage_error\",\"stage\":\"atomic_rename\"}");
    return false;
  }
  return true;
}

bool WavRecorder::writeHeader(uint32_t dataBytes) {
  if (!file_) return false;
  uint8_t header[kWavHeaderBytes];
  encodeWavHeader(header, dataBytes);
  return file_.write(header, sizeof(header)) == sizeof(header);
}

bool WavRecorder::finalizePartialAudio(Print &log) {
  if (dataBytes_ == 0) {
    log.println("{\"event\":\"recording_error\",\"stage\":\"empty_audio\"}");
    return false;
  }
  if (fs_->exists(finalPath_) && !fs_->remove(finalPath_)) return false;
  if (!fs_->rename(partialPath_, finalPath_)) {
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
  if (!fs_->exists(audio) && fs_->exists(partial)) {
    File interrupted = fs_->open(partial, "r+");
    if (!interrupted || interrupted.size() <= kWavHeaderBytes) {
      if (interrupted) interrupted.close();
      return false;
    }
    const uint32_t bytes = static_cast<uint32_t>(interrupted.size() - kWavHeaderBytes);
    uint8_t header[kWavHeaderBytes];
    encodeWavHeader(header, bytes);
    const bool patched = interrupted.seek(0) &&
                         interrupted.write(header, sizeof(header)) == sizeof(header);
    interrupted.flush();
    interrupted.close();
    if (!patched || !fs_->rename(partial, audio)) return false;
  }
  File recoveredAudio = fs_->open(audio, FILE_READ);
  if (!recoveredAudio || recoveredAudio.size() <= kWavHeaderBytes) {
    if (recoveredAudio) recoveredAudio.close();
    return false;
  }
  const uint32_t recoveredBytes =
      static_cast<uint32_t>(recoveredAudio.size() - kWavHeaderBytes);
  recoveredAudio.close();

  recordingId_ = recordingId;
  createdAt_ = recoveredAt;
  directory_ = stagingDirectory;
  partialPath_ = partial;
  finalPath_ = audio;
  dataBytes_ = recoveredBytes;
  if (!writeCapsuleMetadata(log, recoveredAt) ||
      !writeProcessingMetadata(log, "queued", 2, nullptr, nullptr) ||
      !commitStagingDirectory(log)) {
    return false;
  }
  log.printf("{\"event\":\"recording_recovered\",\"id\":\"%s\",\"duration_ms\":%lu}\n",
             recordingId_.c_str(), static_cast<unsigned long>(durationMs()));
  return true;
}

}  // namespace pokepod
