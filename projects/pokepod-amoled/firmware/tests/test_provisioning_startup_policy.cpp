#include <cassert>
#include <cstring>

#include "ProvisioningStartupPolicy.h"

using namespace pokepod;

int main() {
  ProvisioningStartupPolicy policy;
  assert(policy.phase() == ProvisioningStartupPhase::idle);
  assert(policy.request(1000));
  assert(policy.phase() == ProvisioningStartupPhase::requested);
  assert(!policy.request(1001));
  assert(policy.pending());
  assert(policy.visible());
  assert(policy.ownsWifi());

  assert(policy.update(1031, true) == ProvisioningStartupAction::none);
  assert(policy.update(1032, true) ==
         ProvisioningStartupAction::quiesceRadio);
  assert(policy.phase() == ProvisioningStartupPhase::quiescing);
  assert(policy.update(1151, true) == ProvisioningStartupAction::none);
  assert(policy.update(1152, false) == ProvisioningStartupAction::none);
  assert(policy.update(1152, true) ==
         ProvisioningStartupAction::startPortal);
  assert(policy.update(1153, true) == ProvisioningStartupAction::none);
  policy.finishStart(true);
  assert(policy.active());
  assert(!policy.request(1200));
  policy.reset();

  assert(policy.request(2000));
  assert(policy.update(2032, false) ==
         ProvisioningStartupAction::quiesceRadio);
  assert(policy.update(4499, false) == ProvisioningStartupAction::none);
  assert(policy.update(4500, false) ==
         ProvisioningStartupAction::failTimeout);
  assert(policy.failed());
  assert(policy.visible());
  policy.reset();

  const uint32_t nearWrap = 0xfffffff0U;
  assert(policy.request(nearWrap));
  assert(policy.update(nearWrap + 31U, true) ==
         ProvisioningStartupAction::none);
  assert(policy.update(nearWrap + 32U, true) ==
         ProvisioningStartupAction::quiesceRadio);
  assert(policy.update(nearWrap + 151U, true) ==
         ProvisioningStartupAction::none);
  assert(policy.update(nearWrap + 152U, true) ==
         ProvisioningStartupAction::startPortal);
  policy.finishStart(false);
  assert(policy.failed());

  assert(std::strcmp(provisioningStartupPhaseName(
                         ProvisioningStartupPhase::requested),
                     "requested") == 0);
  assert(std::strcmp(provisioningStartupPhaseName(
                         ProvisioningStartupPhase::quiescing),
                     "quiescing") == 0);
  return 0;
}
