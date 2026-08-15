#include <cassert>

#include "PsramDegradedPolicy.h"

using namespace pokepod;

int main() {
  const PsramAllocationDecision ok = psramAllocationSucceeded(4096);
  assert(ok.state == PsramServiceState::ready);
  assert(ok.bytes == 4096);
  assert(!psramDegraded(ok.state));

  const PsramAllocationDecision failed = psramAllocationFailed(8192);
  assert(failed.state == PsramServiceState::degraded);
  assert(failed.bytes == 8192);
  assert(psramDegraded(failed.state));
  assert(!psramMayFallbackToInternal());
  return 0;
}
