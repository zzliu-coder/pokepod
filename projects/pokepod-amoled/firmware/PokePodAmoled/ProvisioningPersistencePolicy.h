#pragma once

#include <stdint.h>

#include "ProvisioningLogCodec.h"
#include "ProvisioningProbeCodec.h"

namespace pokepod {

// Only breadcrumbs that are useful after a reset are written to NVS. Page,
// request and after-stage probes remain in RAM and are still emitted to the
// live USB log.
constexpr bool provisioningProbePersists(ProvisioningProbeStage stage) {
  switch (stage) {
    case ProvisioningProbeStage::beforeModeAp:
    case ProvisioningProbeStage::beforeSoftAp:
    case ProvisioningProbeStage::beforeDnsStart:
    case ProvisioningProbeStage::beforeServerBegin:
    case ProvisioningProbeStage::portalStopped:
      return true;
    default:
      return false;
  }
}

constexpr bool provisioningLogPersists(ProvisioningLogStage stage) {
  switch (stage) {
    case ProvisioningLogStage::portalRequested:
    case ProvisioningLogStage::radioModeStarted:
    case ProvisioningLogStage::accessPointStarted:
    case ProvisioningLogStage::portalStarted:
    case ProvisioningLogStage::connectStarted:
    case ProvisioningLogStage::connected:
    case ProvisioningLogStage::configSaved:
    case ProvisioningLogStage::failed:
    case ProvisioningLogStage::portalStopped:
      return true;
    default:
      return false;
  }
}

constexpr uint16_t kProvisioningMaxPersistentWritesPerSession = 24;

}  // namespace pokepod
