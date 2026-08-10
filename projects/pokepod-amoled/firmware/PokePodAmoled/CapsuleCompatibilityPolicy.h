#pragma once

#include <stdint.h>
#include <string.h>

namespace pokepod {

struct CapsuleWireMetadata {
  int capsuleSchemaVersion = -1;
  int processingSchemaVersion = -1;
  int processingRevision = -1;
  int64_t durationMs = -1;
  const char *status = nullptr;
  const char *audioFile = nullptr;
  const char *audioFormat = nullptr;
  int sampleRateHz = -1;
  int channels = -1;
  int bitsPerSample = -1;
};

inline bool capsuleStringEquals(const char *left, const char *right) {
  return left != nullptr && right != nullptr && strcmp(left, right) == 0;
}

inline bool supportedCapsuleStatus(const char *value) {
  static const char *const values[] = {
      "recording", "recorded", "queued", "transcribing", "raw_ready",
      "correcting", "ready", "failed"};
  if (value == nullptr) return false;
  for (const char *known : values) {
    if (strcmp(value, known) == 0) return true;
  }
  return false;
}

inline bool supportedCapsuleAudio(const CapsuleWireMetadata &metadata) {
  const bool m4a = capsuleStringEquals(metadata.audioFile, "audio.m4a");
  const bool wav = capsuleStringEquals(metadata.audioFile, "audio.wav");
  if (metadata.processingSchemaVersion == 1) return m4a;
  if (metadata.processingSchemaVersion != 2 || (!m4a && !wav) ||
      metadata.sampleRateHz < 8000 || metadata.sampleRateHz > 192000 ||
      metadata.channels < 1 || metadata.channels > 8 ||
      metadata.bitsPerSample < 8 || metadata.bitsPerSample > 32) {
    return false;
  }
  if (m4a) return capsuleStringEquals(metadata.audioFormat, "m4a-aac-lc");
  return capsuleStringEquals(metadata.audioFormat, "wav-pcm-s16le") &&
      metadata.bitsPerSample == 16;
}

inline bool capsuleRecordWritable(const CapsuleWireMetadata &metadata) {
  return metadata.capsuleSchemaVersion == 1 &&
      (metadata.processingSchemaVersion == 1 ||
       metadata.processingSchemaVersion == 2) &&
      metadata.processingRevision >= 0 && metadata.durationMs >= 0 &&
      supportedCapsuleStatus(metadata.status) &&
      supportedCapsuleAudio(metadata);
}

}  // namespace pokepod
