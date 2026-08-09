#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace pokepod {

constexpr uint32_t kProvisioningLogMagic = 0x314C5050;  // PPL1.
constexpr uint16_t kProvisioningLogVersion = 1;
constexpr size_t kProvisioningLogCapacity = 16;
constexpr uint16_t kProvisioningReasonStorage = 65001;
constexpr uint16_t kProvisioningReasonScanStart = 65002;
constexpr uint16_t kProvisioningReasonScanTimeout = 65003;
constexpr uint16_t kProvisioningReasonScanFailed = 65004;
constexpr uint16_t kProvisioningReasonInvalidInput = 65005;
constexpr uint16_t kProvisioningReasonPortalFailed = 65006;

enum class ProvisioningLogStage : uint8_t {
  portalStarted = 1,
  scanStarted = 2,
  scanFinished = 3,
  connectStarted = 4,
  connected = 5,
  configSaved = 6,
  failed = 7,
  portalStopped = 8,
};

enum class ProvisioningLogOutcome : uint8_t {
  info = 0,
  success = 1,
  failure = 2,
};

#pragma pack(push, 1)
struct StoredProvisioningLogRecord {
  uint32_t sequence;
  uint32_t epoch;
  uint32_t elapsedMs;
  uint16_t reason;
  int16_t rssi;
  uint8_t stage;
  uint8_t outcome;
  uint8_t attempt;
  uint8_t reserved;
  char ssid[33];
};

struct StoredProvisioningLog {
  uint32_t magic;
  uint16_t version;
  uint8_t count;
  uint8_t next;
  uint32_t nextSequence;
  StoredProvisioningLogRecord records[kProvisioningLogCapacity];
  uint32_t crc32;
};
#pragma pack(pop)

static_assert(sizeof(StoredProvisioningLog) < 1536,
              "provisioning diagnostics must remain a small NVS blob");

inline uint32_t provisioningLogCrc32(const uint8_t *data, size_t length) {
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

inline void initializeProvisioningLog(StoredProvisioningLog &log) {
  std::memset(&log, 0, sizeof(log));
  log.magic = kProvisioningLogMagic;
  log.version = kProvisioningLogVersion;
  log.nextSequence = 1;
}

inline void finalizeProvisioningLog(StoredProvisioningLog &log) {
  log.crc32 = provisioningLogCrc32(
      reinterpret_cast<const uint8_t *>(&log),
      offsetof(StoredProvisioningLog, crc32));
}

inline bool provisioningLogHasTerminator(const char *value,
                                         size_t capacity) {
  for (size_t index = 0; index < capacity; ++index) {
    if (value[index] == '\0') return true;
  }
  return false;
}

inline bool validateProvisioningLog(const StoredProvisioningLog &log) {
  if (log.magic != kProvisioningLogMagic ||
      log.version != kProvisioningLogVersion ||
      log.count > kProvisioningLogCapacity ||
      log.next >= kProvisioningLogCapacity ||
      log.nextSequence == 0 ||
      log.crc32 != provisioningLogCrc32(
          reinterpret_cast<const uint8_t *>(&log),
          offsetof(StoredProvisioningLog, crc32))) {
    return false;
  }
  for (size_t index = 0; index < kProvisioningLogCapacity; ++index) {
    const StoredProvisioningLogRecord &record = log.records[index];
    if (!provisioningLogHasTerminator(record.ssid, sizeof(record.ssid)) ||
        record.stage > static_cast<uint8_t>(ProvisioningLogStage::portalStopped) ||
        record.outcome > static_cast<uint8_t>(ProvisioningLogOutcome::failure)) {
      return false;
    }
  }
  return true;
}

inline void appendProvisioningLog(StoredProvisioningLog &log,
                                  StoredProvisioningLogRecord record) {
  record.sequence = log.nextSequence++;
  if (log.nextSequence == 0) log.nextSequence = 1;
  log.records[log.next] = record;
  log.next = static_cast<uint8_t>((log.next + 1) % kProvisioningLogCapacity);
  if (log.count < kProvisioningLogCapacity) ++log.count;
}

inline const StoredProvisioningLogRecord *provisioningLogNewest(
    const StoredProvisioningLog &log, size_t newestOffset) {
  if (newestOffset >= log.count) return nullptr;
  const size_t newest = (log.next + kProvisioningLogCapacity - 1) %
      kProvisioningLogCapacity;
  const size_t index = (newest + kProvisioningLogCapacity - newestOffset) %
      kProvisioningLogCapacity;
  return &log.records[index];
}

}  // namespace pokepod
