#include <cassert>

#include "../PokePodAmoled/LinkServiceCoordinator.h"

int main() {
  using namespace pokepod;
  LinkServiceCoordinator coordinator;
  assert(coordinator.acquire(LinkTransport::usb));
  assert(coordinator.acquire(LinkTransport::usb));
  assert(coordinator.busyFor(LinkTransport::wifi));
  assert(!coordinator.acquire(LinkTransport::wifi));
  coordinator.release(LinkTransport::wifi);
  assert(coordinator.owner() == LinkTransport::usb);
  coordinator.release(LinkTransport::usb);
  assert(coordinator.acquire(LinkTransport::wifi));
  assert(coordinator.busyFor(LinkTransport::usb));
  coordinator.release(LinkTransport::wifi);
  assert(coordinator.owner() == LinkTransport::none);
  return 0;
}
