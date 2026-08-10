#include <cassert>

#include "../PokePodAmoled/LinkServiceCoordinator.h"
#include "../PokePodAmoled/UsbCdcSessionState.h"

int main() {
  using namespace pokepod;

  UsbCdcSessionState session;
  LinkServiceCoordinator coordinator;
  const bool physicalUsbMounted = true;

  session.reset();
  assert(physicalUsbMounted);
  assert(!session.active());
  assert(!session.takeClosed());

  // Opening the Mac CDC descriptor asserts DTR and owns the USB Link lease.
  session.lineState(true);
  assert(session.active());
  assert(coordinator.acquire(LinkTransport::usb));
  assert(coordinator.busyFor(LinkTransport::wifi));

  // Closing the descriptor deasserts DTR while the cable remains mounted.
  session.lineState(false);
  assert(physicalUsbMounted);
  assert(!session.active());
  assert(session.takeClosed());
  // This is the main-loop effect of PokePodLinkService::disconnect().
  coordinator.release(LinkTransport::usb);
  assert(!session.takeClosed());
  assert(coordinator.owner() == LinkTransport::none);

  // Either transport can begin after the cancelled USB session releases.
  assert(coordinator.acquire(LinkTransport::wifi));
  coordinator.release(LinkTransport::wifi);
  assert(coordinator.acquire(LinkTransport::usb));
  coordinator.release(LinkTransport::usb);

  // A fast open/close between two loop iterations still leaves a close event.
  session.lineState(true);
  session.lineState(false);
  assert(!session.active());
  assert(session.takeClosed());

  // An open CDC session has no inactivity deadline; large legitimate transfers
  // stay owned until DTR falls or USB physically disconnects.
  session.lineState(true);
  assert(session.active());
  assert(!session.takeClosed());
  session.disconnected();
  session.lineState(false);
  assert(!session.active());
  assert(session.takeClosed());
  assert(!session.takeClosed());

  // A stale USB close event can never release a Wi-Fi-owned coordinator.
  assert(coordinator.acquire(LinkTransport::wifi));
  session.lineState(true);
  session.lineState(false);
  assert(session.takeClosed());
  coordinator.release(LinkTransport::usb);
  assert(coordinator.owner() == LinkTransport::wifi);
  coordinator.release(LinkTransport::wifi);
  return 0;
}
