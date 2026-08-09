#include <assert.h>

#include "BleSingleConnectionPolicy.h"

using namespace pokepod;

int main() {
  BleSingleConnectionPolicy policy;
  assert(!policy.hasCurrent());
  assert(policy.connect(kInvalidBleConnectionId) ==
         BleConnectDecision::rejectSecondary);
  assert(!policy.hasCurrent());

  assert(policy.connect(11) == BleConnectDecision::accepted);
  assert(policy.hasCurrent());
  assert(policy.currentConnectionId() == 11);
  assert(policy.commandAllowed(11));
  assert(policy.authenticationAllowed(11));

  // A second controller connection is rejected without replacing the Mac
  // that already owns the voice service.
  assert(policy.connect(22) == BleConnectDecision::rejectSecondary);
  assert(policy.currentConnectionId() == 11);
  assert(!policy.commandAllowed(22));
  assert(!policy.authenticationAllowed(22));

  // Delayed callbacks from the rejected connection have no side effects.
  assert(!policy.disconnect(22));
  assert(policy.hasCurrent());
  assert(policy.currentConnectionId() == 11);

  // Entering pairing while connected defers advertising until the accepted
  // connection's own disconnect callback arrives.
  policy.requestPairingAfterDisconnect();
  assert(policy.pairingPending());
  assert(!policy.consumePairingAfterDisconnect());
  assert(policy.disconnect(11));
  assert(policy.consumePairingAfterDisconnect());
  assert(!policy.pairingPending());
  assert(!policy.hasCurrent());

  assert(policy.connect(33) == BleConnectDecision::accepted);
  assert(policy.connect(33) == BleConnectDecision::alreadyCurrent);
  policy.requestPairingAfterDisconnect();
  policy.cancelPairingRequest();
  assert(policy.disconnect(33));
  assert(!policy.consumePairingAfterDisconnect());
  return 0;
}
