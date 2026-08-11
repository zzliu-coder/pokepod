#include <assert.h>

#include "../PokePodAmoled/LinkCapsuleTransactionGate.h"

using namespace pokepod;

int main() {
  LinkCapsuleTransactionGate gate;
  // USB has no external deadline.
  gate.beginOperation(nullptr);
  assert(gate.permits(100));
  gate.cancel();
  assert(!gate.permits(101));

  AbsoluteLinkDeadlineGate wireless;
  wireless.arm(1000, 300000);
  gate.beginOperation(&wireless);
  assert(gate.permits(300999));
  assert(!gate.permits(301000));
  gate.reset();
  return 0;
}
