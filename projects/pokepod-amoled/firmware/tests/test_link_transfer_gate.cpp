#include <cassert>
#include <cstddef>
#include <cstdint>

#include "../PokePodAmoled/LinkTransferGate.h"

int main() {
  using namespace pokepod;

  class CancellationProbe final : public LinkTransferCancellationSink {
   public:
    void cancelForTransferDeadline() override { ++calls; }
    size_t calls = 0;
  };

  AbsoluteLinkDeadlineGate wifi;
  CancellationProbe cancellation;
  wifi.attachCancellationSink(&cancellation);
  wifi.arm(0, 300000);
  assert(wifi.permits(299000));

  // Model a large audio read that starts at 299 seconds and checks the same
  // absolute gate before every 250 ms chunk. Exactly four chunks may start;
  // the chunk at 300 seconds is rejected with no drain grace.
  size_t chunks = 0;
  for (uint32_t nowMs = 299000; nowMs <= 301000; nowMs += 250) {
    if (!linkTransferPermitted(&wifi, nowMs)) break;
    ++chunks;
  }
  assert(chunks == 4);
  assert(cancellation.calls == 1);
  assert(!wifi.permits(300000));
  assert(cancellation.calls == 1);
  assert(wifi.expired(300000));

  // A null gate is the USB contract: Wi-Fi's deadline never cancels CDC.
  assert(linkTransferPermitted(nullptr, 300000));
  assert(linkTransferPermitted(nullptr, UINT32_MAX));

  wifi.cancel();
  assert(!wifi.permits(1));
  assert(cancellation.calls == 1);
  wifi.detachCancellationSink(&cancellation);

  AbsoluteLinkDeadlineGate wrapped;
  wrapped.arm(UINT32_MAX - 99, 200);
  assert(wrapped.permits(99));
  assert(!wrapped.permits(100));
  return 0;
}
