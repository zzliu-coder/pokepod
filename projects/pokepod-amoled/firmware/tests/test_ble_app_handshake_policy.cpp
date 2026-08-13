#include <cassert>
#include <cstdint>

#include "../PokePodAmoled/BleAppHandshakePolicy.h"

using pokepod::BleAppHandshakePolicy;

int main() {
  BleAppHandshakePolicy timeout;
  timeout.connected(1000);
  assert(!timeout.requestDisconnect(15999, false));
  assert(timeout.requestDisconnect(16000, false));
  assert(timeout.disconnectPending());
  assert(!timeout.requestDisconnect(16249, false));
  assert(timeout.requestDisconnect(16250, false));
  timeout.ready();
  assert(timeout.requestDisconnect(16500, true));
  assert(!timeout.requestDisconnect(
      16000 + BleAppHandshakePolicy::kCleanupDeadlineMs, false));
  assert(timeout.recoveryRequired());
  timeout.disconnected();
  assert(!timeout.disconnectPending());
  assert(!timeout.recoveryRequired());

  BleAppHandshakePolicy ready;
  ready.connected(500);
  ready.ready();
  assert(!ready.requestDisconnect(500 + BleAppHandshakePolicy::kTimeoutMs,
                                  false));
  ready.disconnected();
  assert(!ready.requestDisconnect(UINT32_MAX, false));

  BleAppHandshakePolicy pairing;
  pairing.connected(0);
  assert(!pairing.requestDisconnect(BleAppHandshakePolicy::kTimeoutMs, true));
  assert(pairing.requestDisconnect(BleAppHandshakePolicy::kTimeoutMs, false));

  BleAppHandshakePolicy wrap;
  constexpr uint32_t start = UINT32_MAX - 1000;
  wrap.connected(start);
  assert(!wrap.requestDisconnect(start + BleAppHandshakePolicy::kTimeoutMs - 1,
                                 false));
  assert(wrap.requestDisconnect(start + BleAppHandshakePolicy::kTimeoutMs,
                                false));
  return 0;
}
