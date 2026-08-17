#include <cassert>

#include "ProvisioningPersistencePolicy.h"

using namespace pokepod;

int main() {
  assert(provisioningProbePersists(ProvisioningProbeStage::beforeModeAp));
  assert(provisioningProbePersists(ProvisioningProbeStage::beforeSoftAp));
  assert(provisioningProbePersists(ProvisioningProbeStage::beforeDnsStart));
  assert(provisioningProbePersists(
      ProvisioningProbeStage::beforeServerBegin));
  assert(provisioningProbePersists(ProvisioningProbeStage::portalStopped));
  assert(!provisioningProbePersists(
      ProvisioningProbeStage::beforeRequestParse));
  assert(!provisioningProbePersists(ProvisioningProbeStage::afterPageSend));

  assert(provisioningLogPersists(ProvisioningLogStage::portalRequested));
  assert(provisioningLogPersists(ProvisioningLogStage::failed));
  assert(provisioningLogPersists(ProvisioningLogStage::portalStopped));
  assert(!provisioningLogPersists(ProvisioningLogStage::scanFinished));
  assert(kProvisioningMaxPersistentWritesPerSession >= 8);
  return 0;
}
