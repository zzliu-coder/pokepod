#include "FirmwareUpdateSession.h"

#include <cstring>

#if defined(ARDUINO_ARCH_ESP32)
  #include <esp_err.h>
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

}  // namespace

void FirmwareUpdateSession::resetTextState() {
  expectedBytes_ = 0;
  receivedBytes_ = 0;
  memset(expectedSha256_, 0, sizeof(expectedSha256_));
  memset(actualSha256_, 0, sizeof(actualSha256_));
  memset(error_, 0, sizeof(error_));
}

bool FirmwareUpdateSession::fail(const char *message) {
  abort();
  if (message == nullptr) message = "firmware update failed";
  strncpy(error_, message, sizeof(error_) - 1);
  error_[sizeof(error_) - 1] = '\0';
  return false;
}

bool FirmwareUpdateSession::begin(uint32_t expectedBytes,
                                  const char *expectedSha256) {
  if (active()) return fail("firmware update already active");
  resetTextState();
  if (!FirmwareUpdatePolicy::validImageSize(expectedBytes) ||
      !copySha256(expectedSha256, expectedSha256_, sizeof(expectedSha256_))) {
    return fail("invalid firmware metadata");
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
}

}  // namespace pokepod
