#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace pokepod {

constexpr uint32_t kWirelessIdentityMagic = 0x31495357U;  // WSI1
constexpr uint16_t kWirelessIdentityVersion = 1;
constexpr size_t kWirelessCertificateCapacity = 1024;
constexpr size_t kWirelessPrivateKeyCapacity = 256;

#pragma pack(push, 1)
struct StoredWirelessSyncIdentity {
  uint32_t magic = kWirelessIdentityMagic;
  uint16_t version = kWirelessIdentityVersion;
  uint8_t paired = 0;
  uint8_t reserved = 0;
  char pairingId[37] = {};
  uint8_t secret[32] = {};
  uint16_t certificateBytes = 0;
  uint16_t privateKeyBytes = 0;
  uint8_t certificate[kWirelessCertificateCapacity] = {};
  uint8_t privateKey[kWirelessPrivateKeyCapacity] = {};
  uint32_t crc32 = 0;
};
#pragma pack(pop)

inline uint32_t wirelessIdentityCrc32(const uint8_t *data, size_t size) {
  uint32_t crc = 0xffffffffU;
  for (size_t index = 0; index < size; ++index) {
    crc ^= data[index];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^ (0xedb88320U &
          static_cast<uint32_t>(-static_cast<int32_t>(crc & 1U)));
    }
  }
  return ~crc;
}

inline void finalizeWirelessIdentityBlob(StoredWirelessSyncIdentity &value) {
  value.crc32 = 0;
  value.crc32 = wirelessIdentityCrc32(
      reinterpret_cast<const uint8_t *>(&value),
      offsetof(StoredWirelessSyncIdentity, crc32));
}

inline bool validUuidText(const char *value) {
  if (value == nullptr || strlen(value) != 36) return false;
  for (size_t index = 0; index < 36; ++index) {
    if (index == 8 || index == 13 || index == 18 || index == 23) {
      if (value[index] != '-') return false;
    } else {
      const char c = value[index];
      if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
  }
  return true;
}

inline bool validateWirelessIdentityBlob(
    const StoredWirelessSyncIdentity &value) {
  return value.magic == kWirelessIdentityMagic &&
      value.version == kWirelessIdentityVersion && value.paired <= 1 &&
      validUuidText(value.pairingId) &&
      value.certificateBytes > 0 &&
      value.certificateBytes <= kWirelessCertificateCapacity &&
      value.privateKeyBytes > 0 &&
      value.privateKeyBytes <= kWirelessPrivateKeyCapacity &&
      wirelessIdentityCrc32(reinterpret_cast<const uint8_t *>(&value),
          offsetof(StoredWirelessSyncIdentity, crc32)) == value.crc32;
}

inline void formatUuidBytes(const uint8_t bytes[16], char output[37]) {
  static constexpr char hex[] = "0123456789abcdef";
  size_t cursor = 0;
  for (size_t index = 0; index < 16; ++index) {
    if (index == 4 || index == 6 || index == 8 || index == 10) {
      output[cursor++] = '-';
    }
    output[cursor++] = hex[bytes[index] >> 4];
    output[cursor++] = hex[bytes[index] & 0x0f];
  }
  output[cursor] = '\0';
}

inline void formatPokePodDeviceId(uint64_t hardwareId, char output[24]) {
  snprintf(output, 24, "pokepod-%04x%08x",
           static_cast<unsigned>((hardwareId >> 32) & 0xffff),
           static_cast<unsigned>(hardwareId & 0xffffffff));
}

}  // namespace pokepod
