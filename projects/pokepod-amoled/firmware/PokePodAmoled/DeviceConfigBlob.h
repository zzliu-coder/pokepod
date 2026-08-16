#pragma once

#include <cstddef>
#include <cstdint>

namespace pokepod {

constexpr size_t kMaximumRememberedWifiNetworks = 5;
constexpr uint32_t kDeviceConfigMagic = 0x31434650;  // PFC1, little-endian.
constexpr uint16_t kDeviceConfigVersion = 1;
constexpr uint8_t kStoredBluetoothLegacyEnabled = 0;
constexpr uint8_t kStoredBluetoothEnabled = 1;
constexpr uint8_t kStoredBluetoothDisabled = 2;
// reserved[1] was zero in the original v1 blob. Keep zero as the legacy
// random behavior and use the remaining values for an explicit provisioning
// password policy without changing the blob size, version, or CRC contract.
constexpr uint8_t kStoredProvisioningPasswordLegacy = 0;
constexpr uint8_t kStoredProvisioningPasswordRandom = 1;
constexpr uint8_t kStoredProvisioningPasswordFixed88888888 = 2;

enum class ProvisioningPasswordMode : uint8_t {
  legacy = kStoredProvisioningPasswordLegacy,
  random = kStoredProvisioningPasswordRandom,
  fixed88888888 = kStoredProvisioningPasswordFixed88888888,
};

constexpr ProvisioningPasswordMode kDefaultProvisioningPasswordMode =
    ProvisioningPasswordMode::fixed88888888;

inline ProvisioningPasswordMode migrateProvisioningPasswordMode(
    ProvisioningPasswordMode mode) {
  return mode == ProvisioningPasswordMode::legacy
      ? kDefaultProvisioningPasswordMode : mode;
}

#pragma pack(push, 1)
struct StoredWifiCredential {
  char ssid[33];
  char password[64];
};

struct StoredDeviceConfig {
  uint32_t magic;
  uint16_t version;
  uint8_t wifiCount;
  uint8_t wifiEnabled;
  uint8_t raiseToWake;
  uint8_t reserved[3];
  StoredWifiCredential wifi[kMaximumRememberedWifiNetworks];
  char secretId[129];
  char secretKey[129];
  char hotwordId[129];
  uint32_t crc32;
};
#pragma pack(pop)

static_assert(sizeof(StoredDeviceConfig) < 1024,
              "device configuration must remain a small atomic NVS blob");
static_assert(sizeof(StoredDeviceConfig) == 888,
              "config_v1 layout size must remain backward compatible");

inline uint32_t deviceConfigCrc32(const uint8_t *data, size_t length) {
  uint32_t crc = 0xffffffffU;
  for (size_t index = 0; index < length; ++index) {
    crc ^= data[index];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1U) ^ (0xedb88320U &
          static_cast<uint32_t>(-static_cast<int32_t>(crc & 1U)));
    }
  }
  return ~crc;
}

inline bool deviceConfigHasTerminator(const char *value, size_t capacity) {
  for (size_t index = 0; index < capacity; ++index) {
    if (value[index] == '\0') return true;
  }
  return false;
}

inline void finalizeDeviceConfigBlob(StoredDeviceConfig &stored) {
  stored.crc32 = deviceConfigCrc32(
      reinterpret_cast<const uint8_t *>(&stored),
      offsetof(StoredDeviceConfig, crc32));
}

inline bool storedDeviceConfigBluetoothEnabled(
    const StoredDeviceConfig &stored) {
  return stored.reserved[0] != kStoredBluetoothDisabled;
}

inline ProvisioningPasswordMode storedDeviceConfigProvisioningPasswordMode(
    const StoredDeviceConfig &stored) {
  return static_cast<ProvisioningPasswordMode>(stored.reserved[1]);
}

inline bool validProvisioningPasswordMode(ProvisioningPasswordMode mode) {
  return mode == ProvisioningPasswordMode::legacy ||
      mode == ProvisioningPasswordMode::random ||
      mode == ProvisioningPasswordMode::fixed88888888;
}

inline bool validateDeviceConfigBlob(const StoredDeviceConfig &stored) {
  if (stored.magic != kDeviceConfigMagic ||
      stored.version != kDeviceConfigVersion ||
      stored.wifiCount > kMaximumRememberedWifiNetworks ||
      stored.wifiEnabled > 1 || stored.raiseToWake > 1 ||
      stored.reserved[0] > kStoredBluetoothDisabled ||
      stored.reserved[1] > kStoredProvisioningPasswordFixed88888888 ||
      stored.crc32 != deviceConfigCrc32(
          reinterpret_cast<const uint8_t *>(&stored),
          offsetof(StoredDeviceConfig, crc32)) ||
      !deviceConfigHasTerminator(stored.secretId, sizeof(stored.secretId)) ||
      !deviceConfigHasTerminator(stored.secretKey, sizeof(stored.secretKey)) ||
      !deviceConfigHasTerminator(stored.hotwordId, sizeof(stored.hotwordId))) {
    return false;
  }
  for (size_t index = 0; index < stored.wifiCount; ++index) {
    if (!deviceConfigHasTerminator(stored.wifi[index].ssid,
                                   sizeof(stored.wifi[index].ssid)) ||
        !deviceConfigHasTerminator(stored.wifi[index].password,
                                   sizeof(stored.wifi[index].password)) ||
        stored.wifi[index].ssid[0] == '\0') {
      return false;
    }
  }
  return true;
}

}  // namespace pokepod
