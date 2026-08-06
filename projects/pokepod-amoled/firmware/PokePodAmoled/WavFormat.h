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

inline void encodeWavHeader(uint8_t *header, uint32_t dataBytes) {
  memset(header, 0, kWavHeaderBytes);
  memcpy(header, "RIFF", 4);
  putLittleEndian32(header + 4, 36 + dataBytes);
  memcpy(header + 8, "WAVEfmt ", 8);
  putLittleEndian32(header + 16, 16);
  putLittleEndian16(header + 20, 1);
  putLittleEndian16(header + 22, kAudioChannels);
  putLittleEndian32(header + 24, kAudioSampleRate);
  putLittleEndian32(header + 28, static_cast<uint32_t>(kAudioByteRate));
  putLittleEndian16(header + 32,
                    static_cast<uint16_t>(kAudioChannels * kAudioBytesPerSample));
  putLittleEndian16(header + 34, kAudioBitsPerSample);
  memcpy(header + 36, "data", 4);
  putLittleEndian32(header + 40, dataBytes);
}

constexpr uint32_t audioDurationMs(uint32_t dataBytes) {
  return kAudioByteRate == 0 ? 0 : static_cast<uint32_t>(
      (static_cast<uint64_t>(dataBytes) * 1000ULL) / kAudioByteRate);
}

}  // namespace pokepod
