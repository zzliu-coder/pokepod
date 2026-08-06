#include "WavRecorder.h"

#include <SD_MMC.h>

#include "WavFormat.h"

namespace pokepod {

bool WavRecorder::start(fs::FS &fs, Print &log, const String &recordingId) {
  if (recording_) return false;
  fs_ = &fs;
  recordingId_ = recordingId;
  directory_ = "/PokePod/recordings/" + recordingId_;
  partialPath_ = directory_ + "/audio.wav.part";
  finalPath_ = directory_ + "/audio.wav";

  fs_->mkdir("/PokePod");
  fs_->mkdir("/PokePod/recordings");
  if (!fs_->mkdir(directory_) && !fs_->exists(directory_)) {
    log.println("{\"event\":\"recording_error\",\"stage\":\"mkdir\"}");
    return false;
  }
  if ((fs_->exists(partialPath_) && !fs_->remove(partialPath_)) ||
      (fs_->exists(finalPath_) && !fs_->remove(finalPath_))) {
    log.println("{\"event\":\"recording_error\",\"stage\":\"remove_stale_audio\"}");
    return false;
  }

  file_ = fs_->open(partialPath_, FILE_WRITE);
  if (!file_) {
    log.println("{\"event\":\"recording_error\",\"stage\":\"open\"}");
    return false;
  }
  dataBytes_ = 0;
  if (!writeHeader(0)) {
    file_.close();
    fs_->remove(partialPath_);
    return false;
  }
  recording_ = true;
  log.printf("{\"event\":\"recording_started\",\"id\":\"%s\"}\n", recordingId_.c_str());
  return true;
}

bool WavRecorder::append(const uint8_t *data, size_t length, Print &log) {
  if (!recording_ || !file_) return false;
  const size_t written = file_.write(data, length);
  dataBytes_ += static_cast<uint32_t>(written);
  if (written != length) {
    log.printf("{\"event\":\"recording_error\",\"stage\":\"short_write\",\"expected\":%u,\"actual\":%u}\n",
               static_cast<unsigned>(length), static_cast<unsigned>(written));
    stop(log);
    return false;
  }
  if (durationMs() >= kMaxRecordingMs) return stop(log);
  return true;
}

bool WavRecorder::stop(Print &log) {
  if (!recording_) return false;
  recording_ = false;
  file_.flush();
  bool ok = file_.seek(0);
  if (ok) ok = writeHeader(dataBytes_);
  file_.flush();
  file_.close();
  if (ok) {
    if (fs_->exists(finalPath_)) fs_->remove(finalPath_);
    ok = fs_->rename(partialPath_, finalPath_);
  }
  if (ok) ok = writeMetadata(log);
  log.printf("{\"event\":\"recording_stopped\",\"ok\":%s,\"duration_ms\":%lu,\"bytes\":%lu,\"path\":\"%s\"}\n",
             ok ? "true" : "false", static_cast<unsigned long>(durationMs()),
             static_cast<unsigned long>(dataBytes_), finalPath_.c_str());
  return ok;
}

uint32_t WavRecorder::durationMs() const {
  return audioDurationMs(dataBytes_);
}

bool WavRecorder::writeHeader(uint32_t dataBytes) {
  if (!file_) return false;
  uint8_t header[kWavHeaderBytes];
  encodeWavHeader(header, dataBytes);
  return file_.write(header, sizeof(header)) == sizeof(header);
}

bool WavRecorder::writeMetadata(Print &log) {
  const String path = directory_ + "/recording.json.part";
  const String finalMetadata = directory_ + "/recording.json";
  if (fs_->exists(path) && !fs_->remove(path)) {
    log.println("{\"event\":\"recording_error\",\"stage\":\"remove_stale_metadata\"}");
    return false;
  }
  File metadata = fs_->open(path, FILE_WRITE);
  if (!metadata) return false;
  const size_t expected = metadata.printf(
      "{\n  \"schemaVersion\": 1,\n  \"id\": \"%s\",\n  \"source\": \"pokepod-amoled\",\n  \"audioFile\": \"audio.wav\",\n  \"sampleRate\": %lu,\n  \"channels\": %u,\n  \"bitsPerSample\": %u,\n  \"durationMs\": %lu\n}\n",
      recordingId_.c_str(), static_cast<unsigned long>(kAudioSampleRate),
      kAudioChannels, kAudioBitsPerSample, static_cast<unsigned long>(durationMs()));
  const bool wrote = expected > 0 && metadata.getWriteError() == 0;
  metadata.flush();
  metadata.close();
  if (!wrote) {
    fs_->remove(path);
    log.println("{\"event\":\"recording_error\",\"stage\":\"metadata_write\"}");
    return false;
  }
  if (fs_->exists(finalMetadata)) fs_->remove(finalMetadata);
  const bool ok = fs_->rename(path, finalMetadata);
  if (!ok) log.println("{\"event\":\"recording_error\",\"stage\":\"metadata_commit\"}");
  return ok;
}

}  // namespace pokepod
