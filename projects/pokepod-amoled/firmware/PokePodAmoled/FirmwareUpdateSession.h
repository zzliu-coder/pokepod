#pragma once

#include <stddef.h>
#include <stdint.h>

#include "FirmwareImageIdentity.h"
#include "FirmwareUpdatePolicy.h"

#if defined(ARDUINO_ARCH_ESP32)
  #include <esp_ota_ops.h>
  #include <mbedtls/sha256.h>
#endif

namespace pokepod {

enum class FirmwareUpdateState : uint8_t {
  idle,
  receiving,
  committed,
};

// USB Link owns the request lifecycle; this object owns only the alternate
// OTA partition handle and the image digest.  It never touches the active
// partition and it aborts without changing boot selection on every failure.
class FirmwareUpdateSession {
 public:
  bool begin(uint32_t expectedBytes, const char *expectedSha256,
             const char *expectedSourceRevision,
             const char *expectedFirmwareVersion,
             const char *expectedAppElfSha256);
  bool writeChunk(const uint8_t *data, size_t bytes);
  bool finish();
  void abort();

  FirmwareUpdateState state() const { return state_; }
  bool active() const { return state_ == FirmwareUpdateState::receiving; }
  bool committed() const { return state_ == FirmwareUpdateState::committed; }
  uint32_t expectedBytes() const { return expectedBytes_; }
  uint32_t receivedBytes() const { return receivedBytes_; }
  const char *expectedSha256() const { return expectedSha256_; }
  const char *actualSha256() const { return actualSha256_; }
  const char *error() const { return error_; }
  const FirmwareImageIdentity &imageIdentity() const { return identity_; }

 private:
  void resetTextState();
  void resetIdentityState();
  bool fail(const char *message);
  void formatDigest(const uint8_t digest[32]);
  bool inspectIdentity(const uint8_t *data, size_t bytes);
  bool validateReceivedIdentity();
  bool validateCandidateAppElfSha256();
  bool copyExpectedText(const char *source, char *destination,
                        size_t capacity, const char *fieldName);

  FirmwareUpdateState state_ = FirmwareUpdateState::idle;
  uint32_t expectedBytes_ = 0;
  uint32_t receivedBytes_ = 0;
  char expectedSha256_[FirmwareUpdatePolicy::kSha256HexBytes + 1] = {};
  char actualSha256_[FirmwareUpdatePolicy::kSha256HexBytes + 1] = {};
  char error_[64] = {};
  char expectedSourceRevision_[kFirmwareImageSourceBytes] = {};
  char expectedFirmwareVersion_[kFirmwareImageFirmwareVersionBytes] = {};
  char expectedAppElfSha256_[kFirmwareImageSha256Bytes] = {};
  FirmwareImageIdentity identity_{};
  uint8_t identityCandidate_[sizeof(FirmwareImageIdentity)] = {};
  size_t identityCandidateBytes_ = 0;
  size_t identityMagicBytes_ = 0;
  uint8_t identityCount_ = 0;

#if defined(ARDUINO_ARCH_ESP32)
  esp_ota_handle_t handle_ = 0;
  const esp_partition_t *target_ = nullptr;
  mbedtls_sha256_context sha_{};
  bool shaInitialized_ = false;
#endif
};

}  // namespace pokepod
