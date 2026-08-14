#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "ProvisioningLogCodec.h"

namespace pokepod {

constexpr uint32_t kProvisioningProbeMagic = 0x31525050;  // PPR1.
constexpr uint16_t kProvisioningProbeVersion = 1;

enum class ProvisioningProbeStage : uint8_t {
  none = 0,
  prepareEntered = 1,
  portalRequested = 2,
  beforeModeAp = 3,
  afterModeAp = 4,
  beforeSoftAp = 5,
  afterSoftAp = 6,
  beforePowerSaveOff = 7,
  afterPowerSaveOff = 8,
  beforeRouteInstall = 9,
  afterRouteInstall = 10,
  beforeDnsStart = 11,
  afterDnsStart = 12,
  beforeServerBegin = 13,
  afterServerBegin = 14,
  beforeRequestParse = 15,
  afterRequestParse = 16,
  beforePageBuild = 17,
  afterPageBuild = 18,
  beforePageSend = 19,
  afterPageSend = 20,
  portalStopped = 21,
};

#pragma pack(push, 1)
struct StoredProvisioningProbe {
  uint32_t magic;
  uint16_t version;
  uint8_t stage;
  uint8_t reserved;
  uint32_t sequence;
  uint32_t uptimeMs;
  uint32_t internalFree;
  uint32_t internalLargest;
  uint32_t psramFree;
  uint32_t psramLargest;
  uint32_t psramTotal;
  uint16_t recordResetReason;
  uint16_t reserved2;
  uint32_t crc32;
};
#pragma pack(pop)

static_assert(sizeof(StoredProvisioningProbe) == 44,
              "provisioning probe must remain a small fixed NVS record");

inline void initializeProvisioningProbe(StoredProvisioningProbe &probe) {
  std::memset(&probe, 0, sizeof(probe));
  probe.magic = kProvisioningProbeMagic;
  probe.version = kProvisioningProbeVersion;
}

inline void finalizeProvisioningProbe(StoredProvisioningProbe &probe) {
  probe.crc32 = provisioningLogCrc32(
      reinterpret_cast<const uint8_t *>(&probe),
      offsetof(StoredProvisioningProbe, crc32));
}

inline bool validateProvisioningProbe(const StoredProvisioningProbe &probe) {
  return probe.magic == kProvisioningProbeMagic &&
      probe.version == kProvisioningProbeVersion &&
      probe.stage <= static_cast<uint8_t>(
          ProvisioningProbeStage::portalStopped) &&
      probe.crc32 == provisioningLogCrc32(
          reinterpret_cast<const uint8_t *>(&probe),
          offsetof(StoredProvisioningProbe, crc32));
}

inline const char *provisioningProbeStageKey(ProvisioningProbeStage stage) {
  switch (stage) {
    case ProvisioningProbeStage::none: return "none";
    case ProvisioningProbeStage::prepareEntered: return "prepare_entered";
    case ProvisioningProbeStage::portalRequested: return "portal_requested";
    case ProvisioningProbeStage::beforeModeAp: return "before_mode_ap";
    case ProvisioningProbeStage::afterModeAp: return "after_mode_ap";
    case ProvisioningProbeStage::beforeSoftAp: return "before_softap";
    case ProvisioningProbeStage::afterSoftAp: return "after_softap";
    case ProvisioningProbeStage::beforePowerSaveOff:
      return "before_power_save_off";
    case ProvisioningProbeStage::afterPowerSaveOff:
      return "after_power_save_off";
    case ProvisioningProbeStage::beforeRouteInstall:
      return "before_route_install";
    case ProvisioningProbeStage::afterRouteInstall:
      return "after_route_install";
    case ProvisioningProbeStage::beforeDnsStart: return "before_dns_start";
    case ProvisioningProbeStage::afterDnsStart: return "after_dns_start";
    case ProvisioningProbeStage::beforeServerBegin:
      return "before_server_begin";
    case ProvisioningProbeStage::afterServerBegin:
      return "after_server_begin";
    case ProvisioningProbeStage::beforeRequestParse:
      return "before_request_parse";
    case ProvisioningProbeStage::afterRequestParse:
      return "after_request_parse";
    case ProvisioningProbeStage::beforePageBuild: return "before_page_build";
    case ProvisioningProbeStage::afterPageBuild: return "after_page_build";
    case ProvisioningProbeStage::beforePageSend: return "before_page_send";
    case ProvisioningProbeStage::afterPageSend: return "after_page_send";
    case ProvisioningProbeStage::portalStopped: return "portal_stopped";
  }
  return "unknown";
}

}  // namespace pokepod
