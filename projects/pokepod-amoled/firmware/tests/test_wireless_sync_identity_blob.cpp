#include <cassert>
#include <cstring>

#include "../PokePodAmoled/WirelessSyncIdentityBlob.h"

int main() {
  using namespace pokepod;
  StoredWirelessSyncIdentity identity;
  std::strcpy(identity.pairingId, "6dc5d4b6-2618-49a4-94fb-874cfb0c6d81");
  identity.certificateBytes = 4;
  identity.privateKeyBytes = 3;
  identity.certificate[0] = 0x30;
  identity.privateKey[0] = 0x30;
  finalizeWirelessIdentityBlob(identity);
  assert(validateWirelessIdentityBlob(identity));
  identity.secret[2] ^= 1;
  assert(!validateWirelessIdentityBlob(identity));

  char deviceId[24];
  formatPokePodDeviceId(0x112233445566ULL, deviceId);
  assert(std::strcmp(deviceId, "pokepod-112233445566") == 0);
  return 0;
}
