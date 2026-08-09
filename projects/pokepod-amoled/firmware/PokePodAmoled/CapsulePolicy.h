#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace pokepod {

constexpr const char *kCapsuleRoot = "/PokeCapsule";
constexpr const char *kCapsuleInbox = "/PokeCapsule/Inbox";
constexpr const char *kCapsuleArchive = "/PokeCapsule/Archive";
constexpr const char *kCapsuleStaging = "/PokeCapsule/.staging";
constexpr const char *kCapsuleTrash = "/PokeCapsule/.trash";
constexpr const char *kCapsuleSystem = "/PokeCapsule/.system";
constexpr const char *kCapsuleWavFile = "audio.wav";
constexpr const char *kCapsuleWavFormat = "wav-pcm-s16le";
constexpr const char *kCapsuleArchiveMetadata = "archive.json";

constexpr bool isHexDigit(char value) {
  return (value >= '0' && value <= '9') ||
         (value >= 'a' && value <= 'f') ||
         (value >= 'A' && value <= 'F');
}

constexpr bool isUuid(const char *value) {
  if (value == nullptr) return false;
  for (size_t index = 0; index < 36; ++index) {
    const bool separator = index == 8 || index == 13 || index == 18 || index == 23;
    if (separator ? value[index] != '-' : !isHexDigit(value[index])) return false;
  }
  return value[36] == '\0';
}

inline char hexNibble(uint8_t value) {
  return value < 10 ? static_cast<char>('0' + value)
                    : static_cast<char>('a' + value - 10);
}

inline void formatUuidV4(const uint8_t randomBytes[16], char output[37]) {
  uint8_t bytes[16];
  for (size_t index = 0; index < 16; ++index) bytes[index] = randomBytes[index];
  bytes[6] = static_cast<uint8_t>((bytes[6] & 0x0f) | 0x40);
  bytes[8] = static_cast<uint8_t>((bytes[8] & 0x3f) | 0x80);

  size_t outputIndex = 0;
  for (size_t index = 0; index < 16; ++index) {
    if (index == 4 || index == 6 || index == 8 || index == 10) {
      output[outputIndex++] = '-';
    }
    output[outputIndex++] = hexNibble(bytes[index] >> 4);
    output[outputIndex++] = hexNibble(bytes[index] & 0x0f);
  }
  output[outputIndex] = '\0';
}

constexpr bool safeCapsuleFileName(const char *value) {
  if (value == nullptr || value[0] == '\0' || value[0] == '.') return false;
  for (size_t index = 0; value[index] != '\0'; ++index) {
    const char c = value[index];
    if (c == '/' || c == '\\' || c == '\0') return false;
  }
  return true;
}

constexpr bool capsuleStatusNeedsStartupRequeue(const char *value) {
  if (value == nullptr) return false;
  constexpr char interrupted[] = "transcribing";
  size_t index = 0;
  while (interrupted[index] != '\0' && value[index] == interrupted[index]) ++index;
  return interrupted[index] == '\0' && value[index] == '\0';
}

inline bool safeArchiveOriginalFolder(const char *value) {
  if (value == nullptr || value[0] == '\0' || value[0] == '.' ||
      value[0] == '/' || strlen(value) > 160 ||
      strcmp(value, "Archive") == 0 || strncmp(value, "Archive/", 8) == 0) {
    return false;
  }
  size_t segmentStart = 0;
  for (size_t index = 0;; ++index) {
    const char c = value[index];
    if (c == '\\' || (c != '\0' && static_cast<unsigned char>(c) < 0x20)) {
      return false;
    }
    if (c == '/' || c == '\0') {
      const size_t length = index - segmentStart;
      if (length == 0 || value[segmentStart] == '.' ||
          (length == 2 && value[segmentStart] == '.' &&
           value[segmentStart + 1] == '.')) {
        return false;
      }
      if (c == '\0') break;
      segmentStart = index + 1;
    }
  }
  return true;
}

}  // namespace pokepod
