#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "FirmwarePolicy.h"

namespace pokepod {

constexpr size_t kWavHeaderBytes = 44;

inline void putLittleEndian16(uint8_t *target, uint16_t value) {
  target[0] = value & 0xff;
  target[1] = (value >> 8) & 0xff;
}

inline void putLittleEndian32(uint8_t *target, uint32_t value) {
  target[0] = value & 0xff;
  target[1] = (value >> 8) & 0xff;
  target[2] = (value >> 16) & 0xff;
  target[3] = (value >> 24) & 0xff;
}

constexpr uint16_t getLittleEndian16(const uint8_t *source) {
  return static_cast<uint16_t>(source[0]) |
         (static_cast<uint16_t>(source[1]) << 8);
}

constexpr uint32_t getLittleEndian32(const uint8_t *source) {
  return static_cast<uint32_t>(source[0]) |
         (static_cast<uint32_t>(source[1]) << 8) |
         (static_cast<uint32_t>(source[2]) << 16) |
         (static_cast<uint32_t>(source[3]) << 24);
}

inline bool validCapsuleWavHeader(const uint8_t *header, size_t size,
                                  uint32_t fileBytes,
                                  uint32_t &dataBytes) {
  if (header == nullptr || size < kWavHeaderBytes || fileBytes < kWavHeaderBytes ||
      memcmp(header, "RIFF", 4) != 0 || memcmp(header + 8, "WAVEfmt ", 8) != 0 ||
      getLittleEndian32(header + 16) != 16 || getLittleEndian16(header + 20) != 1 ||
      getLittleEndian16(header + 22) != kCapsuleChannels ||
      getLittleEndian32(header + 24) != kCapsuleSampleRate ||
      getLittleEndian16(header + 34) != kCapsuleBitsPerSample ||
      memcmp(header + 36, "data", 4) != 0) {
    return false;
  }
  dataBytes = getLittleEndian32(header + 40);
  return dataBytes > 0 && dataBytes % 2 == 0 &&
         dataBytes <= fileBytes - kWavHeaderBytes;
}

inline void encodeWavHeader(uint8_t *header, uint32_t dataBytes) {
  memset(header, 0, kWavHeaderBytes);
  memcpy(header, "RIFF", 4);
  putLittleEndian32(header + 4, 36 + dataBytes);
  memcpy(header + 8, "WAVEfmt ", 8);
  putLittleEndian32(header + 16, 16);
  putLittleEndian16(header + 20, 1);
  putLittleEndian16(header + 22, kCapsuleChannels);
  putLittleEndian32(header + 24, kCapsuleSampleRate);
  putLittleEndian32(header + 28, static_cast<uint32_t>(kCapsuleByteRate));
  putLittleEndian16(header + 32,
                    static_cast<uint16_t>(kCapsuleChannels * kCapsuleBytesPerSample));
  putLittleEndian16(header + 34, kCapsuleBitsPerSample);
  memcpy(header + 36, "data", 4);
  putLittleEndian32(header + 40, dataBytes);
}

constexpr uint32_t audioDurationMs(uint32_t dataBytes) {
  return kCapsuleByteRate == 0 ? 0 : static_cast<uint32_t>(
      (static_cast<uint64_t>(dataBytes) * 1000ULL) / kCapsuleByteRate);
}

}  // namespace pokepod
