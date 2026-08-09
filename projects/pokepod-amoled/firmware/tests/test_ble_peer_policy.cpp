#include <assert.h>

#include "BlePeerPolicy.h"

using namespace pokepod;

int main() {
  BlePeerPolicy policy;

  // The one previously bonded Mac may reconnect without opening pairing mode.
  policy.connected(true);
  assert(policy.currentPeerBonded());
  assert(policy.securityAllowed(false));
  assert(policy.commandAllowed(true, true, false));

  // A different Mac cannot borrow the existence of that bond.
  policy.disconnected();
  policy.connected(false);
  assert(!policy.currentPeerBonded());
  assert(!policy.securityAllowed(false));
  assert(!policy.commandAllowed(true, true, false));

  // An unknown Mac is accepted only while the user opened pairing mode.
  assert(policy.securityAllowed(true));
  assert(!policy.commandAllowed(true, false, true));
  policy.authenticatedAndBonded();
  assert(policy.currentPeerBonded());
  assert(policy.securityAllowed(false));

  // Forgetting the Mac immediately removes the authorization grant.
  policy.forgotBonds();
  assert(!policy.currentPeerBonded());
  assert(!policy.securityAllowed(false));
  assert(!policy.commandAllowed(true, true, false));
  return 0;
}
