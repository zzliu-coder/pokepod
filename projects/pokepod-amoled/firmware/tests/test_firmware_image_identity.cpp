#include <cassert>
#include <cstring>

#include "FirmwareImageIdentity.h"

using namespace pokepod;

int main() {
  static_assert(sizeof(FirmwareImageIdentity) == 203);
  FirmwareImageIdentity identity{};
  std::memcpy(identity.magic, kFirmwareImageMagic, sizeof(identity.magic));
  identity.schemaVersion = kFirmwareImageIdentitySchema;
  identity.structBytes = sizeof(FirmwareImageIdentity);
  std::strcpy(identity.product, kFirmwareProductName);
  std::strcpy(identity.firmwareVersion, "2.0.0");
  std::strcpy(identity.sourceRevision,
              "0123456789abcdef0123456789abcdef01234567");
  std::strcpy(identity.sourceTree,
              "fedcba9876543210fedcba9876543210fedcba98");
  std::strcpy(identity.appElfSha256, "unknown");
  assert(firmwareImageIdentityValid(identity));

  identity.sourceDirty = 1;
  assert(firmwareImageIdentityValid(identity));
  identity.sourceTree[0] = 'x';
  assert(!firmwareImageIdentityValid(identity));
  identity.sourceTree[0] = 'f';
  identity.schemaVersion = 1;
  assert(!firmwareImageIdentityValid(identity));
  identity.schemaVersion = kFirmwareImageIdentitySchema;
  std::memset(identity.appElfSha256, '0', 64);
  identity.appElfSha256[64] = '\0';
  assert(firmwareImageIdentityValid(identity));
  identity.appElfSha256[0] = 'z';
  assert(!firmwareImageIdentityValid(identity));
  return 0;
}
