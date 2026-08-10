#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace pokepod {

constexpr uint32_t kPowerLogMagic = 0x31445750;  // PWD1.
constexpr uint16_t kPowerLogVersion = 1;
// Keep the complete JSON export below the 4 KiB Link v2 control-frame limit
// even when every numeric field is at its maximum printable width.
constexpr size_t kPowerLogCapacity = 10;

enum class PowerLogEvent : uint8_t {
  boot = 1,
  sleepBlocked = 2,
  lightSleepWake = 3,
  lightSleepError = 4,
  deepSleepIntent = 5,
  deepSleepArmError = 6,
  safeShutdown = 7,
  automaticScreenWake = 8,
};

enum PowerLogFlag : uint16_t {
  kPowerFlagScreenOn = 1U << 0,
  kPowerFlagAutomaticWake = 1U << 1,
  kPowerFlagUsbHost = 1U << 2,
  kPowerFlagVbus = 1U << 3,
  kPowerFlagAutomaticPm = 1U << 4,
  kPowerFlagBleModemSleep = 1U << 5,
};

#pragma pack(push, 1)
struct StoredPowerLogRecord {
  uint32_t sequence;
  uint32_t epoch;
  uint32_t uptimeMs;
  uint32_t durationMs;
  uint32_t blockers;
  uint32_t detail;
  uint64_t ext1WakeMask;
  int32_t error;
  uint16_t flags;
  uint16_t resetReason;
  uint8_t event;
  uint8_t mode;
  uint8_t wakeCause;
  int8_t batteryPercent;
};

struct StoredPowerLog {
  uint32_t magic;
  uint16_t version;
  uint8_t count;
  uint8_t next;
  uint32_t nextSequence;
  StoredPowerLogRecord records[kPowerLogCapacity];
  uint32_t crc32;
};
#pragma pack(pop)

static_assert(sizeof(StoredPowerLogRecord) == 44,
              "power record layout is part of the persistent codec");
static_assert(sizeof(StoredPowerLog) < 768,
              "power diagnostics must remain a small NVS blob");

inline uint32_t powerLogCrc32(const uint8_t *data, size_t length) {
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

inline void initializePowerLog(StoredPowerLog &log) {
  std::memset(&log, 0, sizeof(log));
  log.magic = kPowerLogMagic;
  log.version = kPowerLogVersion;
  log.nextSequence = 1;
}

inline void finalizePowerLog(StoredPowerLog &log) {
  log.crc32 = powerLogCrc32(
      reinterpret_cast<const uint8_t *>(&log),
      offsetof(StoredPowerLog, crc32));
}

inline bool validatePowerLog(const StoredPowerLog &log) {
  if (log.magic != kPowerLogMagic || log.version != kPowerLogVersion ||
      log.count > kPowerLogCapacity || log.next >= kPowerLogCapacity ||
      log.nextSequence == 0 ||
      log.crc32 != powerLogCrc32(
          reinterpret_cast<const uint8_t *>(&log),
          offsetof(StoredPowerLog, crc32))) {
    return false;
  }
  for (size_t index = 0; index < kPowerLogCapacity; ++index) {
    if (log.records[index].event >
            static_cast<uint8_t>(PowerLogEvent::automaticScreenWake) ||
        log.records[index].mode > 5) {
      return false;
    }
  }
  return true;
}

inline bool shouldPersistBlockedObservation(
    uint32_t nowMs, uint32_t lastPersistedAtMs, uint8_t stage,
    uint8_t lastStage, uint32_t blockers, uint32_t lastBlockers,
    uint32_t rateLimitMs) {
  if (blockers == 0 || (stage == lastStage && blockers == lastBlockers)) {
    return false;
  }
  return lastPersistedAtMs == 0 ||
      static_cast<uint32_t>(nowMs - lastPersistedAtMs) >= rateLimitMs;
}

inline bool shouldPersistLightWake(uint32_t wakeCountThisBoot,
                                   uint32_t durationMs) {
  return wakeCountThisBoot == 1 || (wakeCountThisBoot % 16) == 0 ||
      durationMs < 250;
}

inline bool shouldPersistAutomaticScreenWake(uint32_t wakeCountThisBoot) {
  return wakeCountThisBoot == 1 || (wakeCountThisBoot % 8) == 0;
}

inline void appendPowerLog(StoredPowerLog &log,
                           StoredPowerLogRecord record) {
  record.sequence = log.nextSequence++;
  if (log.nextSequence == 0) log.nextSequence = 1;
  log.records[log.next] = record;
  log.next = static_cast<uint8_t>((log.next + 1) % kPowerLogCapacity);
  if (log.count < kPowerLogCapacity) ++log.count;
}

inline const StoredPowerLogRecord *powerLogNewest(
    const StoredPowerLog &log, size_t newestOffset) {
  if (newestOffset >= log.count) return nullptr;
  const size_t newest =
      (log.next + kPowerLogCapacity - 1) % kPowerLogCapacity;
  const size_t index =
      (newest + kPowerLogCapacity - newestOffset) % kPowerLogCapacity;
  return &log.records[index];
}

}  // namespace pokepod
