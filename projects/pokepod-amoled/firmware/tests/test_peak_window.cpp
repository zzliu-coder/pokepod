#include <cassert>

#include "../PokePodAmoled/PeakWindow.h"

int main() {
  pokepod::PeakWindow peak;
  assert(peak.latest() == 0);
  assert(peak.consume() == 0);

  peak.observe(120);
  peak.observe(80);
  peak.observe(320);
  assert(peak.latest() == 320);
  assert(peak.consume() == 320);
  assert(peak.consume() == 0);

  peak.observe(24);
  assert(peak.latest() == 24);
  peak.reset();
  assert(peak.latest() == 0);
  assert(peak.consume() == 0);
  return 0;
}
