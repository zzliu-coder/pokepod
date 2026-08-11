#pragma once

#include <Arduino.h>
#include <stdint.h>

namespace pokepod {

// Read-only metadata projection shared by the production step scanner and its
// host fixture. Mutation remains in CapsuleLibrary/CapsuleTransaction.
struct CapsuleDecodedMetadata {
  String id;
  String title;
  String createdAt;
  String updatedAt;
  String audioFile;
  String audioFormat;
  String status;
  String errorStage;
  String error;
  bool favorite = false;
  bool readOnly = true;
  int capsuleSchemaVersion = -1;
  int processingSchemaVersion = -1;
  int revision = -1;
  int processingRevision = -1;
  uint32_t durationMs = 0;
  uint32_t sampleRateHz = 0;
  uint8_t channels = 0;
  uint8_t bitsPerSample = 0;
};

class CapsuleMetadataCodec {
 public:
  static bool decode(const String &directoryId, const String &capsuleText,
                     const String &processingText,
                     CapsuleDecodedMetadata &decoded);
};

}  // namespace pokepod
