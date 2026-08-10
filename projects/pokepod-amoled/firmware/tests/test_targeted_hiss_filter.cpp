#include <assert.h>
#include <math.h>
#include <stdint.h>

#include <vector>

#include "TargetedHissFilter.h"

using namespace pokepod;

static double rmsTail(const std::vector<int32_t> &samples) {
  const size_t first = samples.size() / 2;
  double energy = 0.0;
  for (size_t index = first; index < samples.size(); ++index) {
    energy += static_cast<double>(samples[index]) * samples[index];
  }
  return sqrt(energy / static_cast<double>(samples.size() - first));
}

static std::vector<int32_t> filterTone(double frequency,
                                       int32_t amplitude = 12000) {
  TargetedHissFilter filter;
  filter.reset();
  std::vector<int32_t> output;
  output.reserve(16000);
  for (size_t index = 0; index < 16000; ++index) {
    const double phase = 2.0 * 3.14159265358979323846 * frequency *
        static_cast<double>(index) / 16000.0;
    output.push_back(filter.process(
        static_cast<int32_t>(sin(phase) * amplitude)));
  }
  return output;
}

int main() {
  const double reference = 12000.0 / sqrt(2.0);
  for (const double frequency : {300.0, 1000.0, 2000.0, 3400.0,
                                 4200.0, 4500.0, 4800.0}) {
    // -1 dB at most through 4.8 kHz; the designed response is about -0.2 dB.
    assert(rmsTail(filterTone(frequency)) > reference * 0.89);
  }
  for (const double frequency : {5300.0, 5500.0, 5700.0, 6000.0,
                                 6300.0, 6500.0}) {
    // At least 30 dB rejection across the measured hiss cluster.
    assert(rmsTail(filterTone(frequency)) < reference / 31.0);
  }
  assert(rmsTail(filterTone(7200.0)) > reference * 0.89);

  TargetedHissFilter clean;
  TargetedHissFilter mixed;
  clean.reset();
  mixed.reset();
  std::vector<int32_t> cleanSpeech;
  std::vector<int32_t> residual;
  cleanSpeech.reserve(16000);
  residual.reserve(16000);
  for (size_t index = 0; index < 16000; ++index) {
    const double speechPhase = 2.0 * 3.14159265358979323846 * 1000.0 *
        static_cast<double>(index) / 16000.0;
    const double hissPhase = 2.0 * 3.14159265358979323846 * 6000.0 *
        static_cast<double>(index) / 16000.0;
    const int32_t speech = static_cast<int32_t>(sin(speechPhase) * 4000);
    const int32_t combined = speech +
        static_cast<int32_t>(sin(hissPhase) * 4000);
    const int32_t cleanOutput = clean.process(speech);
    cleanSpeech.push_back(cleanOutput);
    residual.push_back(mixed.process(combined) - cleanOutput);
  }
  assert(rmsTail(residual) < rmsTail(cleanSpeech) / 30.0);

  // Reset is a hard recording/session boundary: old FIR history cannot leak.
  TargetedHissFilter resetFilter;
  resetFilter.reset();
  resetFilter.process(30000);
  resetFilter.reset();
  for (size_t index = 0; index < TargetedHissFilter::kTaps * 2; ++index) {
    assert(resetFilter.process(0) == 0);
  }
  return 0;
}
