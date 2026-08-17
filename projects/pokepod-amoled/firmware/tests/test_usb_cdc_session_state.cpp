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
  uint32_t closedGeneration = 99;
  assert(!session.takeClosed(closedGeneration));
  assert(closedGeneration == 0);

  // Opening the Mac CDC descriptor asserts DTR and owns the USB Link lease.
  session.lineState(true);
  assert(session.active());
  assert(session.snapshot().generation == 1);
  assert(coordinator.acquire(LinkTransport::usb));
  assert(coordinator.busyFor(LinkTransport::wifi));

  // Closing the descriptor deasserts DTR while the cable remains mounted.
  session.lineState(false);
  assert(physicalUsbMounted);
  assert(!session.active());
  assert(session.takeClosed(closedGeneration));
  assert(closedGeneration == 1);
  // This is the main-loop effect of PokePodLinkService::disconnect().
  coordinator.release(LinkTransport::usb);
  assert(!session.takeClosed(closedGeneration));
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
  assert(session.snapshot().generation == 2);
  assert(session.takeClosed(closedGeneration));
  assert(closedGeneration == 2);

  // If close and reopen both happen before the loop consumes the event, the
  // stale close stays tagged with the old generation and the new session is
  // observable without losing its buffered request.
  session.lineState(true);
  const uint32_t oldGeneration = session.snapshot().generation;
  session.lineState(false);
  session.lineState(true);
  const UsbCdcSessionSnapshot reopened = session.snapshot();
  assert(reopened.active);
  assert(reopened.generation == oldGeneration + 1);
  assert(session.takeClosed(closedGeneration));
  assert(closedGeneration == oldGeneration);

  // An open CDC session has no inactivity deadline; large legitimate transfers
  // stay owned until DTR falls or USB physically disconnects.
  session.lineState(false);
  assert(session.takeClosed(closedGeneration));
  session.lineState(true);
  assert(session.active());
  assert(!session.takeClosed(closedGeneration));
  session.disconnected();
  session.lineState(false);
  assert(!session.active());
  assert(session.takeClosed(closedGeneration));
  assert(!session.takeClosed(closedGeneration));

  // A stale USB close event can never release a Wi-Fi-owned coordinator.
  assert(coordinator.acquire(LinkTransport::wifi));
  session.lineState(true);
  session.lineState(false);
  assert(session.takeClosed(closedGeneration));
  coordinator.release(LinkTransport::usb);
  assert(coordinator.owner() == LinkTransport::wifi);
  coordinator.release(LinkTransport::wifi);
  return 0;
}
