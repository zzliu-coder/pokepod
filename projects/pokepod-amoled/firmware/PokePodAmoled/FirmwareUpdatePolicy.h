#pragma once

#include <stddef.h>
#include <stdint.h>

namespace pokepod {

// The application partitions are fixed at 3 MiB by app3M_fat9M_16MB.  Keep
// this policy independent from ESP-IDF so the host model can prove the
// protocol boundaries without linking an Arduino or OTA implementation.
struct FirmwareUpdatePolicy {
  static constexpr uint32_t kMaximumImageBytes = 0x300000U;
  static constexpr uint32_t kMinimumImageBytes = 1024U;
  static constexpr size_t kSha256HexBytes = 64U;

  static bool validSha256(const char *value) {
    if (value == nullptr) return false;
    for (size_t index = 0; index < kSha256HexBytes; ++index) {
      const char c = value[index];
      if (c == '\0') return false;
      const bool digit = c >= '0' && c <= '9';
      const bool lower = c >= 'a' && c <= 'f';
      const bool upper = c >= 'A' && c <= 'F';
      if (!digit && !lower && !upper) return false;
    }
    return value[kSha256HexBytes] == '\0';
  }

  static bool validImageSize(uint32_t expectedBytes) {
    return expectedBytes >= kMinimumImageBytes &&
        expectedBytes <= kMaximumImageBytes;
  }

  static bool acceptsChunk(uint32_t receivedBytes, uint32_t expectedBytes,
                           size_t chunkBytes) {
    if (!validImageSize(expectedBytes) || chunkBytes == 0) return false;
    return receivedBytes <= expectedBytes &&
        chunkBytes <= static_cast<size_t>(expectedBytes - receivedBytes);
  }

  static bool complete(uint32_t receivedBytes, uint32_t expectedBytes) {
    return validImageSize(expectedBytes) && receivedBytes == expectedBytes;
  }
};

}  // namespace pokepod
