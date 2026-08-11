#include "CapsuleMetadataCodec.h"

#include <cJSON.h>
#include <strings.h>

#include "CapsuleCompatibilityPolicy.h"
#include "CapsulePolicy.h"

namespace pokepod {
namespace {

const char *jsonString(cJSON *root, const char *name) {
  cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
  return cJSON_IsString(item) && item->valuestring != nullptr
      ? item->valuestring : nullptr;
}

int jsonInt(cJSON *root, const char *name, int fallback = -1) {
  cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
  return cJSON_IsNumber(item) ? item->valueint : fallback;
}

bool jsonBool(cJSON *root, const char *name) {
  return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, name));
}

}  // namespace

bool CapsuleMetadataCodec::decode(const String &directoryId,
                                  const String &capsuleText,
                                  const String &processingText,
                                  CapsuleDecodedMetadata &decoded) {
  decoded = CapsuleDecodedMetadata();
  if (!isUuid(directoryId.c_str()) || capsuleText.isEmpty() ||
      processingText.isEmpty()) return false;
  cJSON *capsule = cJSON_ParseWithLength(capsuleText.c_str(),
                                         capsuleText.length());
  cJSON *processing = cJSON_ParseWithLength(processingText.c_str(),
                                            processingText.length());
  if (capsule == nullptr || processing == nullptr) {
    cJSON_Delete(capsule);
    cJSON_Delete(processing);
    return false;
  }

  const char *id = jsonString(capsule, "id");
  const char *processingId = jsonString(processing, "capsuleId");
  const bool valid = isUuid(id) && processingId != nullptr &&
      strcasecmp(id, processingId) == 0 &&
      directoryId.equalsIgnoreCase(String(id));
  if (!valid) {
    cJSON_Delete(capsule);
    cJSON_Delete(processing);
    return false;
  }

  decoded.id = id;
  const char *title = jsonString(capsule, "title");
  const char *createdAt = jsonString(capsule, "createdAt");
  const char *updatedAt = jsonString(capsule, "updatedAt");
  const char *audioFile = jsonString(processing, "audioFile");
  const char *audioFormat = jsonString(processing, "audioFormat");
  const char *wireStatus = jsonString(processing, "status");
  const char *errorStage = jsonString(processing, "errorStage");
  const char *error = jsonString(processing, "error");
  decoded.title = title == nullptr ? "语音胶囊" : title;
  decoded.createdAt = createdAt == nullptr ? "" : createdAt;
  decoded.updatedAt = updatedAt == nullptr ? "" : updatedAt;
  decoded.audioFile = audioFile == nullptr ? "" : audioFile;
  decoded.errorStage = errorStage == nullptr ? "" : errorStage;
  decoded.error = error == nullptr ? "" : error;
  decoded.status = wireStatus == nullptr ? "" : wireStatus;
  decoded.favorite = jsonBool(capsule, "favorite");
  decoded.capsuleSchemaVersion = jsonInt(capsule, "schemaVersion");
  decoded.processingSchemaVersion = jsonInt(processing, "schemaVersion");
  decoded.audioFormat = decoded.processingSchemaVersion == 1
      ? "m4a-aac-lc" : (audioFormat == nullptr ? "" : audioFormat);
  decoded.revision = jsonInt(capsule, "revision", 1);
  decoded.processingRevision = jsonInt(processing, "revision");
  const int durationMs = jsonInt(processing, "durationMs");
  const int sampleRateHz = decoded.processingSchemaVersion == 1
      ? 16000 : jsonInt(processing, "sampleRateHz");
  const int channels = decoded.processingSchemaVersion == 1
      ? 1 : jsonInt(processing, "channels");
  const int bitsPerSample = decoded.processingSchemaVersion == 1
      ? 16 : jsonInt(processing, "bitsPerSample");
  decoded.durationMs = durationMs < 0 ? 0 : static_cast<uint32_t>(durationMs);
  decoded.sampleRateHz = sampleRateHz < 0
      ? 0 : static_cast<uint32_t>(sampleRateHz);
  decoded.channels = channels < 0 ? 0 : static_cast<uint8_t>(channels);
  decoded.bitsPerSample = bitsPerSample < 0
      ? 0 : static_cast<uint8_t>(bitsPerSample);

  CapsuleWireMetadata metadata;
  metadata.capsuleSchemaVersion = decoded.capsuleSchemaVersion;
  metadata.processingSchemaVersion = decoded.processingSchemaVersion;
  metadata.processingRevision = decoded.processingRevision;
  metadata.durationMs = durationMs;
  metadata.status = wireStatus;
  metadata.audioFile = audioFile;
  metadata.audioFormat = audioFormat;
  metadata.sampleRateHz = sampleRateHz;
  metadata.channels = channels;
  metadata.bitsPerSample = bitsPerSample;
  decoded.readOnly = !capsuleRecordWritable(metadata);

  cJSON_Delete(capsule);
  cJSON_Delete(processing);
  return true;
}

}  // namespace pokepod
