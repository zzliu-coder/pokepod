#include <cassert>
#include <cstring>

#include "../PokePodAmoled/ProvisioningPolicy.h"

int main() {
  assert(std::strcmp(pokepod::kProvisioningPassword, "88888888") == 0);
  assert(pokepod::kProvisioningPasswordLength == 8);
  assert(std::strcmp(pokepod::provisioningStateName(
                         pokepod::ProvisioningState::ready), "ready") == 0);
  assert(std::strcmp(pokepod::provisioningStateName(
                         pokepod::ProvisioningState::scanning), "scanning") == 0);
  assert(std::strcmp(pokepod::provisioningStateName(
                         pokepod::ProvisioningState::connecting), "connecting") == 0);
  assert(std::strcmp(pokepod::provisioningStateName(
                         pokepod::ProvisioningState::connected), "connected") == 0);
  assert(std::strcmp(pokepod::provisioningStateName(
                         pokepod::ProvisioningState::error), "error") == 0);
  return 0;
}
