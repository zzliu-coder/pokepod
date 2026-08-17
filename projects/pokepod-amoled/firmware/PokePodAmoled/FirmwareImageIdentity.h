#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "FirmwareVersion.h"

// These values are supplied by firmware/build.sh from the real Git checkout.
// Host tests deliberately use the safe sentinel values below; a production
// build never obtains identity from an arbitrary caller-provided file.
#ifndef POKEPOD_SOURCE_REVISION
#define POKEPOD_SOURCE_REVISION "unknown"
#endif
#ifndef POKEPOD_SOURCE_TREE
#define POKEPOD_SOURCE_TREE "unknown"
#endif
#ifndef POKEPOD_SOURCE_DIRTY
#define POKEPOD_SOURCE_DIRTY 1
#endif
#ifndef POKEPOD_APP_ELF_SHA256
#define POKEPOD_APP_ELF_SHA256 "unknown"
#endif

namespace pokepod {

constexpr char kFirmwareImageMagic[] = "PKPDIMG2";
constexpr char kFirmwareProductName[] = "PokePodAmoled";
constexpr uint16_t kFirmwareImageIdentitySchema = 2;
constexpr size_t kFirmwareImageSourceBytes = 41;
constexpr size_t kFirmwareImageFirmwareVersionBytes = 16;
constexpr size_t kFirmwareImageSha256Bytes = 65;

#pragma pack(push, 1)
struct FirmwareImageIdentity {
  char magic[8];
  uint16_t schemaVersion;
  uint16_t structBytes;
  char product[24];
  char firmwareVersion[kFirmwareImageFirmwareVersionBytes];
  char sourceRevision[kFirmwareImageSourceBytes];
  uint8_t sourceDirty;
  uint8_t reserved[3];
  char sourceTree[kFirmwareImageSourceBytes];
  char appElfSha256[kFirmwareImageSha256Bytes];
};
#pragma pack(pop)

static_assert(sizeof(FirmwareImageIdentity) == 203,
              "firmware image identity is a stable host/device contract");

template <size_t DestinationBytes, size_t SourceBytes>
constexpr void copyFirmwareIdentityText(
    char (&destination)[DestinationBytes],
    const char (&source)[SourceBytes]) {
  const size_t count = SourceBytes < DestinationBytes
      ? SourceBytes : DestinationBytes;
  for (size_t index = 0; index < count; ++index) {
    destination[index] = source[index];
  }
  destination[DestinationBytes - 1] = '\0';
}

constexpr FirmwareImageIdentity makeFirmwareImageIdentity() {
  FirmwareImageIdentity identity{};
  identity.magic[0] = 'P';
  identity.magic[1] = 'K';
  identity.magic[2] = 'P';
  identity.magic[3] = 'D';
  identity.magic[4] = 'I';
  identity.magic[5] = 'M';
  identity.magic[6] = 'G';
  identity.magic[7] = '2';
  identity.schemaVersion = kFirmwareImageIdentitySchema;
  identity.structBytes = sizeof(FirmwareImageIdentity);
  copyFirmwareIdentityText(identity.product, kFirmwareProductName);
  copyFirmwareIdentityText(identity.firmwareVersion, kFirmwareVersion);
  copyFirmwareIdentityText(identity.sourceRevision, POKEPOD_SOURCE_REVISION);
  identity.sourceDirty = POKEPOD_SOURCE_DIRTY ? 1U : 0U;
  copyFirmwareIdentityText(identity.sourceTree, POKEPOD_SOURCE_TREE);
  copyFirmwareIdentityText(identity.appElfSha256, POKEPOD_APP_ELF_SHA256);
  return identity;
}

// Exactly one translation unit owns the marker. Keeping the definition out of
// this header prevents duplicate marker records when Arduino compiles every
// source file in the sketch directory.
extern const FirmwareImageIdentity kFirmwareImageIdentity;

inline size_t firmwareIdentityTextLength(const char *value, size_t capacity) {
  if (value == nullptr) return 0;
  size_t length = 0;
  while (length < capacity && value[length] != '\0') ++length;
  return length;
}

inline bool firmwareIdentityHex(const char *value, size_t capacity,
                                size_t minimumBytes) {
  const size_t length = firmwareIdentityTextLength(value, capacity);
  if (length < minimumBytes || length >= capacity) return false;
  for (size_t index = 0; index < length; ++index) {
    const char c = value[index];
    const bool digit = c >= '0' && c <= '9';
    const bool lower = c >= 'a' && c <= 'f';
    const bool upper = c >= 'A' && c <= 'F';
    if (!digit && !lower && !upper) return false;
  }
  return true;
}

inline bool firmwareIdentityEquals(const char *value, size_t capacity,
                                   const char *literal) {
  if (value == nullptr || literal == nullptr) return false;
  const size_t literalLength = strlen(literal);
  return literalLength < capacity &&
      firmwareIdentityTextLength(value, capacity) == literalLength &&
      memcmp(value, literal, literalLength) == 0;
}

inline bool firmwareImageIdentityValid(const FirmwareImageIdentity &identity) {
  return memcmp(identity.magic, kFirmwareImageMagic, sizeof(identity.magic)) == 0 &&
      identity.schemaVersion == kFirmwareImageIdentitySchema &&
      identity.structBytes == sizeof(FirmwareImageIdentity) &&
      strncmp(identity.product, kFirmwareProductName,
              sizeof(identity.product)) == 0 &&
      firmwareIdentityTextLength(identity.firmwareVersion,
                                 sizeof(identity.firmwareVersion)) > 0 &&
      firmwareIdentityHex(identity.sourceRevision,
                          sizeof(identity.sourceRevision), 40) &&
      (identity.sourceDirty == 0 || identity.sourceDirty == 1) &&
      firmwareIdentityHex(identity.sourceTree, sizeof(identity.sourceTree), 40) &&
      (firmwareIdentityEquals(identity.appElfSha256,
                              sizeof(identity.appElfSha256), "unknown") ||
       firmwareIdentityHex(identity.appElfSha256,
                           sizeof(identity.appElfSha256), 64));
}

}  // namespace pokepod
