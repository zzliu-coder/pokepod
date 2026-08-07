#include <cassert>

#include "../PokePodAmoled/PeakWindow.h"

int main() {
  pokepod::PeakWindow peak;
  uint16_t envelope[pokepod::PeakWindow::kEnvelopeSamples] = {};
  assert(peak.latest() == 0);
  assert(peak.consume() == 0);

  // Four read peaks form one real envelope bucket. Output is chronological
  // and left-padded until the history has filled.
  peak.observe(10);
  peak.observe(40);
  peak.observe(20);
  peak.observe(30);
  peak.observe(70);
  peak.observe(50);
  peak.observe(60);
  peak.observe(55);
  peak.copyEnvelope(envelope, pokepod::PeakWindow::kEnvelopeSamples);
  assert(envelope[pokepod::PeakWindow::kEnvelopeSamples - 2] == 40);
  assert(envelope[pokepod::PeakWindow::kEnvelopeSamples - 1] == 70);

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
  peak.copyEnvelope(envelope, pokepod::PeakWindow::kEnvelopeSamples);
  for (uint16_t value : envelope) assert(value == 0);
  return 0;
}
