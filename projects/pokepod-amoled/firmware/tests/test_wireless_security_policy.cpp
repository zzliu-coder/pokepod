#include <cassert>
#include <cstdint>
#include <string>

#include "../PokePodAmoled/WirelessSecurityPolicy.h"

int main() {
  using namespace pokepod;

  ProvisioningCsrfPolicy csrf;
  csrf.begin("window-one-token", 100, 300);
  assert(csrf.accepts("window-one-token", 100));
  assert(csrf.accepts("window-one-token", 399));
  assert(!csrf.accepts("", 100));
  assert(!csrf.accepts("wrong-token", 100));
  assert(!csrf.accepts("window-one-token", 400));

  csrf.begin("window-two-token", 500, 300);
  assert(!csrf.accepts("window-one-token", 500));
  assert(csrf.accepts("window-two-token", 500));
  csrf.close();
  assert(!csrf.accepts("window-two-token", 501));

  WirelessAuthDeadline authentication;
  assert(!authentication.expired(120000));
  authentication.observeTlsReady(120000);
  assert(authentication.armed());
  assert(authentication.startedAtMs() == 120000);
  assert(!authentication.expired(125999));
  assert(authentication.expired(126000));
  authentication.observeTlsReady(130000);
  assert(authentication.startedAtMs() == 120000);

  authentication.reset();
  authentication.observeTlsReady(UINT32_MAX - 1000);
  assert(!authentication.expired(4998));
  assert(authentication.expired(4999));

  PairingExportRotationPolicy rotation;
  uint32_t secretGeneration = 7;
  assert(!rotation.shouldRotate(false, 1000));
  if (rotation.shouldRotate(true, 1000)) ++secretGeneration;
  assert(secretGeneration == 8);
  if (rotation.shouldRotate(true, 1119)) ++secretGeneration;
  assert(secretGeneration == 8);  // Lost USB response retries same secret.
  if (rotation.shouldRotate(true, 1239)) ++secretGeneration;
  assert(secretGeneration == 9);
  rotation.rotationFailed();
  assert(rotation.shouldRotate(true, 1240));
  return 0;
}
