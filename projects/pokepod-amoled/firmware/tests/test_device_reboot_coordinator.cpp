#include <assert.h>
#include <stdint.h>

#include "../PokePodAmoled/DeviceRebootCoordinator.h"

using namespace pokepod;

int main() {
  DeviceRebootCoordinator reboot;
  assert(!reboot.pending());
  assert(!reboot.request(10, LinkTransport::none));

  // A Wi-Fi service submits into device authority. Destroying or disconnecting
  // that transport has no API that can clear the accepted request.
  assert(reboot.request(1000, LinkTransport::wifi));
  assert(reboot.pending());
  assert(reboot.origin() == LinkTransport::wifi);
  assert(!reboot.request(1001, LinkTransport::usb));
  assert(!reboot.due(1099));
  assert(reboot.due(1100));

  assert(reboot.beginServiceQuiesce());
  assert(reboot.phase() == DeviceRebootPhase::waitingForServices);
  reboot.defer(1100);
  assert(!reboot.due(1119));
  assert(reboot.due(1120));

  // A timeout retries cooperatively without losing the device intent.
  reboot.retryServiceQuiesce(1120);
  assert(reboot.pending());
  assert(reboot.phase() == DeviceRebootPhase::accepted);
  assert(!reboot.due(1219));
  assert(reboot.due(1220));
  assert(reboot.beginServiceQuiesce());
  assert(reboot.markReady());
  assert(reboot.ready());

  // Only the App immediately before ESP.restart acknowledges the authority.
  reboot.acknowledgeRestart();
  assert(!reboot.pending());
  assert(reboot.origin() == LinkTransport::none);

  // USB and Wi-Fi use the same reusable coordinator across device sessions.
  assert(reboot.request(UINT32_MAX - 49U, LinkTransport::usb));
  assert(!reboot.due(49));
  assert(reboot.due(50));
  reboot.acknowledgeRestart();
  return 0;
}
