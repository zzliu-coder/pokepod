#include <cassert>

#include "../PokePodAmoled/LinkLivenessProbe.h"

using namespace pokepod;

int main() {
  LinkLivenessProbe probe;
  assert(probe.observe(6000, true, 9, 1, false, true, true) ==
         LinkLivenessStall::none);

  probe.openSession(7, 100);
  assert(probe.observe(5099, true, 9, 1, false, true, true) ==
         LinkLivenessStall::none);
  assert(probe.observe(5100, true, 9, 1, false, true, true) ==
         LinkLivenessStall::transmit);
  assert(probe.observe(5100, true, 9, 1, false, true, false) ==
         LinkLivenessStall::none);
  probe.recovered(LinkLivenessStall::transmit, 5100);
  assert(probe.snapshot().recoveryCount == 1);
  assert(probe.snapshot().lastRecoveryMs == 5100);
  assert(probe.snapshot().lastStall == LinkLivenessStall::transmit);

  probe.openSession(8, 10000);
  assert(probe.observe(15000, true, 0, 0, true, false, true) ==
         LinkLivenessStall::receive);
  probe.noteProgress(14999);
  assert(probe.observe(15000, true, 9, 0, false, false, true) ==
         LinkLivenessStall::none);

  probe.noteProgress(0xfffffff0U);
  assert(probe.observe(0x00001378U, true, 10, 0, false, false, true) ==
         LinkLivenessStall::operation);
  assert(probe.observe(0x00001378U, true, 10, 0, false, false, false) ==
         LinkLivenessStall::none);
  assert(probe.observe(0x00001378U, false, 10, 1, true, true, true) ==
         LinkLivenessStall::none);

  return 0;
}
