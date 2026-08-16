#include "FirmwareUpdateSession.h"

#include <cstring>

#if defined(ARDUINO_ARCH_ESP32)
  #include <esp_err.h>
  #include <esp_app_desc.h>
  #include <esp_partition.h>
#endif

namespace pokepod {
namespace {

constexpr char kHex[] = "0123456789abcdef";

bool copySha256(const char *source, char *target, size_t capacity) {
  if (!FirmwareUpdatePolicy::validSha256(source) ||
      capacity < FirmwareUpdatePolicy::kSha256HexBytes + 1) {
    return false;
  }
  for (size_t index = 0; index < FirmwareUpdatePolicy::kSha256HexBytes;
       ++index) {
    char value = source[index];
    if (value >= 'A' && value <= 'F') value = static_cast<char>(value - 'A' + 'a');
    target[index] = value;
  }
  target[FirmwareUpdatePolicy::kSha256HexBytes] = '\0';
  return true;
}

bool validSourceRevision(const char *value) {
  if (value == nullptr || strlen(value) != 40) return false;
  for (size_t index = 0; index < 40; ++index) {
    const char c = value[index];
    const bool digit = c >= '0' && c <= '9';
    const bool lower = c >= 'a' && c <= 'f';
    const bool upper = c >= 'A' && c <= 'F';
    if (!digit && !lower && !upper) return false;
  }
  return true;
}

bool validFirmwareVersion(const char *value) {
  if (value == nullptr) return false;
  const size_t length = strlen(value);
  if (length == 0 || length > 15) return false;
  for (size_t index = 0; index < length; ++index) {
    const unsigned char character = static_cast<unsigned char>(value[index]);
    if (character < 0x20 || character >= 0x7f) return false;
  }
  return true;
}

bool copyOptionalText(const char *source, char *target, size_t capacity) {
  if (source == nullptr || target == nullptr || capacity == 0) return false;
  const size_t length = firmwareIdentityTextLength(source, capacity);
  if (length == 0 || length >= capacity) return false;
  memcpy(target, source, length);
  target[length] = '\0';
  return true;
}

}  // namespace

void FirmwareUpdateSession::resetTextState() {
  expectedBytes_ = 0;
  receivedBytes_ = 0;
  memset(expectedSha256_, 0, sizeof(expectedSha256_));
  memset(actualSha256_, 0, sizeof(actualSha256_));
  memset(error_, 0, sizeof(error_));
  memset(expectedSourceRevision_, 0, sizeof(expectedSourceRevision_));
  memset(expectedFirmwareVersion_, 0, sizeof(expectedFirmwareVersion_));
  memset(expectedAppElfSha256_, 0, sizeof(expectedAppElfSha256_));
  resetIdentityState();
}

void FirmwareUpdateSession::resetIdentityState() {
  memset(&identity_, 0, sizeof(identity_));
  memset(identityCandidate_, 0, sizeof(identityCandidate_));
  identityCandidateBytes_ = 0;
  identityMagicBytes_ = 0;
  identityCount_ = 0;
}

bool FirmwareUpdateSession::fail(const char *message) {
  abort();
  if (message == nullptr) message = "firmware update failed";
  strncpy(error_, message, sizeof(error_) - 1);
  error_[sizeof(error_) - 1] = '\0';
  return false;
}

bool FirmwareUpdateSession::copyExpectedText(const char *source,
                                             char *destination,
                                             size_t capacity,
                                             const char *fieldName) {
  if (source == nullptr || source[0] == '\0') return true;
  if (!copyOptionalText(source, destination, capacity)) {
    return fail(fieldName == nullptr ? "invalid expected identity" : fieldName);
  }
  return true;
}

bool FirmwareUpdateSession::begin(uint32_t expectedBytes,
                                  const char *expectedSha256,
                                  const char *expectedSourceRevision,
                                  const char *expectedFirmwareVersion,
                                  const char *expectedAppElfSha256) {
  if (active()) return fail("firmware update already active");
  resetTextState();
  if (!FirmwareUpdatePolicy::validImageSize(expectedBytes) ||
      !copySha256(expectedSha256, expectedSha256_, sizeof(expectedSha256_))) {
    return fail("invalid firmware metadata");
  }
  if (expectedSourceRevision == nullptr || expectedSourceRevision[0] == '\0' ||
      expectedFirmwareVersion == nullptr || expectedFirmwareVersion[0] == '\0' ||
      expectedAppElfSha256 == nullptr || expectedAppElfSha256[0] == '\0' ||
      !validSourceRevision(expectedSourceRevision) ||
      !validFirmwareVersion(expectedFirmwareVersion) ||
      !FirmwareUpdatePolicy::validSha256(expectedAppElfSha256)) {
    return fail("missing firmware identity metadata");
  }
  if (!copyExpectedText(expectedSourceRevision, expectedSourceRevision_,
                        sizeof(expectedSourceRevision_),
                        "invalid expected source revision") ||
      !copyExpectedText(expectedFirmwareVersion, expectedFirmwareVersion_,
                        sizeof(expectedFirmwareVersion_),
                        "invalid expected firmware version") ||
      !copySha256(expectedAppElfSha256, expectedAppElfSha256_,
                  sizeof(expectedAppElfSha256_))) {
    return false;
  }
  for (size_t index = 0; index < strlen(expectedSourceRevision_); ++index) {
    if (expectedSourceRevision_[index] >= 'A' &&
        expectedSourceRevision_[index] <= 'F') {
      expectedSourceRevision_[index] = static_cast<char>(
          expectedSourceRevision_[index] - 'A' + 'a');
    }
  }
#if defined(ARDUINO_ARCH_ESP32)
  const esp_partition_t *running = esp_ota_get_running_partition();
  target_ = esp_ota_get_next_update_partition(running);
  if (running == nullptr || target_ == nullptr ||
      target_->type != ESP_PARTITION_TYPE_APP ||
      target_->address == running->address ||
      target_->size < expectedBytes) {
    target_ = nullptr;
    return fail("no safe OTA partition");
  }
  mbedtls_sha256_init(&sha_);
  if (mbedtls_sha256_starts(&sha_, 0) != 0) {
    mbedtls_sha256_free(&sha_);
    return fail("firmware digest start failed");
  }
  shaInitialized_ = true;
  if (esp_ota_begin(target_, expectedBytes, &handle_) != ESP_OK) {
    mbedtls_sha256_free(&sha_);
    shaInitialized_ = false;
    target_ = nullptr;
    return fail("OTA begin failed");
  }
#else
  return fail("firmware OTA requires ESP32");
#endif

  expectedBytes_ = expectedBytes;
  state_ = FirmwareUpdateState::receiving;
  return true;
}

bool FirmwareUpdateSession::writeChunk(const uint8_t *data, size_t bytes) {
  if (!active() || data == nullptr ||
      !FirmwareUpdatePolicy::acceptsChunk(receivedBytes_, expectedBytes_, bytes)) {
    return fail("invalid firmware chunk");
  }
#if defined(ARDUINO_ARCH_ESP32)
  if (!inspectIdentity(data, bytes)) {
    return fail("firmware identity scan failed");
  }
  if (esp_ota_write(handle_, data, bytes) != ESP_OK ||
      mbedtls_sha256_update(&sha_, data, bytes) != 0) {
    return fail("OTA write failed");
  }
#else
  (void)data;
  (void)bytes;
  return fail("firmware OTA requires ESP32");
#endif
  receivedBytes_ += static_cast<uint32_t>(bytes);
  return true;
}

bool FirmwareUpdateSession::inspectIdentity(const uint8_t *data, size_t bytes) {
  if (data == nullptr || bytes == 0) return false;
  for (size_t index = 0; index < bytes; ++index) {
    const uint8_t value = data[index];
    if (identityCandidateBytes_ != 0) {
      if (identityCandidateBytes_ >= sizeof(identityCandidate_)) {
        identityCandidateBytes_ = 0;
        identityMagicBytes_ = 0;
      } else {
        identityCandidate_[identityCandidateBytes_++] = value;
        if (identityCandidateBytes_ == sizeof(identityCandidate_)) {
          FirmwareImageIdentity candidate{};
          memcpy(&candidate, identityCandidate_, sizeof(candidate));
          if (firmwareImageIdentityValid(candidate)) {
            if (identityCount_ < 0xffU) ++identityCount_;
            if (identityCount_ == 1) identity_ = candidate;
          }
          identityCandidateBytes_ = 0;
          identityMagicBytes_ = 0;
        }
        continue;
      }
    }
    if (value == static_cast<uint8_t>(kFirmwareImageMagic[identityMagicBytes_])) {
      ++identityMagicBytes_;
      if (identityMagicBytes_ == sizeof(kFirmwareImageMagic) - 1U) {
        memcpy(identityCandidate_, kFirmwareImageMagic,
               sizeof(kFirmwareImageMagic) - 1U);
        identityCandidateBytes_ = sizeof(kFirmwareImageMagic) - 1U;
        identityMagicBytes_ = 0;
      }
    } else {
      identityMagicBytes_ = value == static_cast<uint8_t>(kFirmwareImageMagic[0])
          ? 1U : 0U;
    }
  }
  return true;
}

bool FirmwareUpdateSession::validateReceivedIdentity() {
  if (identityCount_ != 1 || !firmwareImageIdentityValid(identity_) ||
      identity_.sourceDirty != 0) {
    return false;
  }
  if (expectedSourceRevision_[0] != '\0' &&
      strncmp(identity_.sourceRevision, expectedSourceRevision_,
              sizeof(identity_.sourceRevision)) != 0) {
    return false;
  }
  if (expectedFirmwareVersion_[0] != '\0' &&
      strncmp(identity_.firmwareVersion, expectedFirmwareVersion_,
              sizeof(identity_.firmwareVersion)) != 0) {
    return false;
  }
  return true;
}

bool FirmwareUpdateSession::validateCandidateAppElfSha256() {
#if defined(ARDUINO_ARCH_ESP32)
  if (target_ == nullptr ||
      !FirmwareUpdatePolicy::validSha256(expectedAppElfSha256_)) {
    return false;
  }
  esp_app_desc_t description{};
  if (esp_ota_get_partition_description(target_, &description) != ESP_OK) {
    return false;
  }
  constexpr char kHex[] = "0123456789abcdef";
  char actual[FirmwareUpdatePolicy::kSha256HexBytes + 1] = {};
  bool hasNonZeroByte = false;
  for (size_t index = 0; index < sizeof(description.app_elf_sha256);
       ++index) {
    const uint8_t value = description.app_elf_sha256[index];
    hasNonZeroByte = hasNonZeroByte || value != 0;
    actual[index * 2] = kHex[value >> 4];
    actual[index * 2 + 1] = kHex[value & 0x0f];
  }
  return hasNonZeroByte && strcmp(actual, expectedAppElfSha256_) == 0;
#else
  return false;
#endif
}

void FirmwareUpdateSession::formatDigest(const uint8_t digest[32]) {
  for (size_t index = 0; index < 32; ++index) {
    actualSha256_[index * 2] = kHex[(digest[index] >> 4) & 0x0f];
    actualSha256_[index * 2 + 1] = kHex[digest[index] & 0x0f];
  }
  actualSha256_[FirmwareUpdatePolicy::kSha256HexBytes] = '\0';
}

bool FirmwareUpdateSession::finish() {
  if (!active() || !FirmwareUpdatePolicy::complete(receivedBytes_, expectedBytes_)) {
    return fail("firmware length mismatch");
  }
#if defined(ARDUINO_ARCH_ESP32)
  uint8_t digest[32] = {};
  if (!shaInitialized_ || mbedtls_sha256_finish(&sha_, digest) != 0) {
    return fail("firmware digest failed");
  }
  mbedtls_sha256_free(&sha_);
  shaInitialized_ = false;
  formatDigest(digest);
  if (strcmp(expectedSha256_, actualSha256_) != 0) {
    (void)esp_ota_abort(handle_);
    handle_ = 0;
    target_ = nullptr;
    return fail("firmware SHA-256 mismatch");
  }
  if (!validateReceivedIdentity()) {
    (void)esp_ota_abort(handle_);
    handle_ = 0;
    target_ = nullptr;
    return fail("firmware image identity mismatch");
  }
  // The marker is embedded before the ESP app descriptor is available, so a
  // production image may carry appElfSha256="unknown" there. The candidate
  // partition descriptor is the authoritative pre-boot value.
  if (!validateCandidateAppElfSha256()) {
    (void)esp_ota_abort(handle_);
    handle_ = 0;
    target_ = nullptr;
    return fail("candidate app ELF SHA-256 mismatch");
  }
  if (esp_ota_end(handle_) != ESP_OK) {
    handle_ = 0;
    target_ = nullptr;
    return fail("OTA image validation failed");
  }
  handle_ = 0;
  if (esp_ota_set_boot_partition(target_) != ESP_OK) {
    target_ = nullptr;
    return fail("OTA boot selection failed");
  }
  target_ = nullptr;
#else
  return fail("firmware OTA requires ESP32");
#endif
  state_ = FirmwareUpdateState::committed;
  error_[0] = '\0';
  return true;
}

void FirmwareUpdateSession::abort() {
#if defined(ARDUINO_ARCH_ESP32)
  if (handle_ != 0) {
    (void)esp_ota_abort(handle_);
    handle_ = 0;
  }
  if (shaInitialized_) {
    mbedtls_sha256_free(&sha_);
    shaInitialized_ = false;
  }
  target_ = nullptr;
#endif
  state_ = FirmwareUpdateState::idle;
  expectedBytes_ = 0;
  receivedBytes_ = 0;
  memset(expectedSha256_, 0, sizeof(expectedSha256_));
  memset(actualSha256_, 0, sizeof(actualSha256_));
  memset(expectedSourceRevision_, 0, sizeof(expectedSourceRevision_));
  memset(expectedFirmwareVersion_, 0, sizeof(expectedFirmwareVersion_));
  memset(expectedAppElfSha256_, 0, sizeof(expectedAppElfSha256_));
  resetIdentityState();
}

}  // namespace pokepod
