#include <cassert>
#include <cstring>

#include "../PokePodAmoled/ProvisioningPolicy.h"

int main() {
  assert(std::strcmp(pokepod::kProvisioningPassword, "88888888") == 0);
  assert(pokepod::kProvisioningPasswordLength == 8);
  return 0;
}
