#include <assert.h>
#include <stdint.h>
#include <cstring>

#include "../PokePodAmoled/ProvisioningPolicy.h"

int main() {
  using namespace pokepod;

  ProvisioningSensitiveConfirmationPolicy confirmation;
  assert(!confirmation.pending());
  assert(!confirmation.begin(ProvisioningSensitiveAction::none, 100));
  assert(confirmation.begin(ProvisioningSensitiveAction::replaceTencent, 100));
  assert(confirmation.pending());
  assert(confirmation.action() ==
         ProvisioningSensitiveAction::replaceTencent);
  assert(confirmation.remainingMs(100) ==
         ProvisioningSensitiveConfirmationPolicy::kConfirmationLifetimeMs);
  assert(!confirmation.begin(ProvisioningSensitiveAction::clearTencent, 101));
  assert(confirmation.remainingMs(30099) == 1);
  assert(confirmation.acceptPhysicalPress(30099));
  assert(!confirmation.pending());
  assert(confirmation.action() == ProvisioningSensitiveAction::none);
  assert(!confirmation.acceptPhysicalPress(30099));

  constexpr uint32_t start = UINT32_MAX - 100U;
  assert(confirmation.begin(ProvisioningSensitiveAction::clearTencent, start));
  const uint32_t beforeDeadline =
      start + ProvisioningSensitiveConfirmationPolicy::kConfirmationLifetimeMs - 1U;
  const uint32_t atDeadline =
      start + ProvisioningSensitiveConfirmationPolicy::kConfirmationLifetimeMs;
  assert(!confirmation.expire(beforeDeadline));
  assert(confirmation.pending());
  assert(confirmation.remainingMs(beforeDeadline) == 1U);
  assert(confirmation.expire(atDeadline));
  assert(!confirmation.pending());
  assert(!confirmation.acceptPhysicalPress(atDeadline));

  assert(std::strcmp(provisioningSensitiveActionName(
                         ProvisioningSensitiveAction::none),
                     "none") == 0);
  assert(std::strcmp(provisioningSensitiveActionName(
                         ProvisioningSensitiveAction::replaceTencent),
                     "replaceTencent") == 0);
  assert(std::strcmp(provisioningSensitiveActionName(
                         ProvisioningSensitiveAction::clearTencent),
                     "clearTencent") == 0);
  return 0;
}
