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

  // Link requests can be handled after the loop captured its timestamp.
  // That stale timestamp must not underflow into an immediate timeout.
  assert(policy.update(999) == ProvisioningStartupAction::none);
  assert(policy.phase() == ProvisioningStartupPhase::requested);
  assert(policy.update(1031) == ProvisioningStartupAction::none);
  assert(policy.update(1032) ==
         ProvisioningStartupAction::quiesceRadio);
  assert(policy.phase() == ProvisioningStartupPhase::quiescing);
  assert(policy.update(2000, false) == ProvisioningStartupAction::none);
  assert(policy.phase() == ProvisioningStartupPhase::quiescing);
  assert(policy.update(1151, true) == ProvisioningStartupAction::none);
  assert(policy.update(1152, true) ==
         ProvisioningStartupAction::switchRadioMode);
  assert(policy.update(1153) == ProvisioningStartupAction::none);
  policy.finishStep(ProvisioningStartupAction::switchRadioMode, true, 1152);
  assert(policy.phase() == ProvisioningStartupPhase::switchingMode);
  assert(policy.update(1151) == ProvisioningStartupAction::none);
  assert(policy.update(1271) == ProvisioningStartupAction::none);
  assert(policy.update(1272) ==
         ProvisioningStartupAction::startAccessPoint);
  policy.finishStep(ProvisioningStartupAction::startAccessPoint, true, 1272);
  assert(policy.phase() == ProvisioningStartupPhase::startingAccessPoint);
  assert(policy.update(1391) == ProvisioningStartupAction::none);
  assert(policy.update(1392) ==
         ProvisioningStartupAction::startPortalServices);
  policy.finishStep(ProvisioningStartupAction::startPortalServices, true, 1392);
  assert(policy.active());
  assert(!policy.request(1400));
  policy.reset();

  assert(policy.request(2000));
  assert(policy.update(2032) ==
         ProvisioningStartupAction::quiesceRadio);
  assert(policy.update(2151) == ProvisioningStartupAction::none);
  assert(policy.update(2152) ==
         ProvisioningStartupAction::switchRadioMode);
  policy.finishStep(ProvisioningStartupAction::switchRadioMode, false, 2152);
  assert(policy.failed());
  assert(policy.visible());
  policy.reset();

  const uint32_t nearWrap = 0xfffffff0U;
  assert(policy.request(nearWrap));
  assert(policy.update(nearWrap + 31U) ==
         ProvisioningStartupAction::none);
  assert(policy.update(nearWrap + 32U) ==
         ProvisioningStartupAction::quiesceRadio);
  assert(policy.update(nearWrap + 151U) ==
         ProvisioningStartupAction::none);
  assert(policy.update(nearWrap + 152U) ==
         ProvisioningStartupAction::switchRadioMode);
  policy.finishStep(ProvisioningStartupAction::switchRadioMode, true,
                    nearWrap + 152U);
  assert(policy.update(nearWrap + 271U) ==
         ProvisioningStartupAction::none);
  assert(policy.update(nearWrap + 272U) ==
         ProvisioningStartupAction::startAccessPoint);
  policy.finishStep(ProvisioningStartupAction::startAccessPoint, false,
                    nearWrap + 272U);
  assert(policy.failed());

  policy.reset();
  assert(policy.request(5000));
  assert(policy.update(5032) == ProvisioningStartupAction::quiesceRadio);
  assert(policy.update(5152) == ProvisioningStartupAction::switchRadioMode);
  policy.finishStep(ProvisioningStartupAction::switchRadioMode, true, 5152);
  assert(policy.update(9000) == ProvisioningStartupAction::failTimeout);
  assert(policy.failed());

  assert(std::strcmp(provisioningStartupPhaseName(
                         ProvisioningStartupPhase::requested),
                     "requested") == 0);
  assert(std::strcmp(provisioningStartupPhaseName(
                         ProvisioningStartupPhase::quiescing),
                     "quiescing") == 0);
  assert(std::strcmp(provisioningStartupPhaseName(
                         ProvisioningStartupPhase::switchingMode),
                     "switching-mode") == 0);
  assert(std::strcmp(provisioningStartupPhaseName(
                         ProvisioningStartupPhase::startingAccessPoint),
                     "starting-access-point") == 0);
  return 0;
}
