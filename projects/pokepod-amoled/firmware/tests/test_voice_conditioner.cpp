#include <assert.h>
#include <math.h>
#include <stdint.h>

#include <vector>

#include "VoiceConditioner.h"

using namespace pokepod;

static double rmsTail(const std::vector<int32_t> &samples) {
  const size_t first = samples.size() / 2;
  double energy = 0.0;
  for (size_t index = first; index < samples.size(); ++index) {
    energy += static_cast<double>(samples[index]) * samples[index];
  }
  return sqrt(energy / static_cast<double>(samples.size() - first));
}

static std::vector<int32_t> conditionTone(double frequency, int32_t amplitude,
                                          VoiceConditionerMetrics *metrics) {
  VoiceConditioner conditioner;
  conditioner.reset(metrics);
  std::vector<int32_t> output;
  output.reserve(16000);
  for (size_t index = 0; index < 16000; ++index) {
    const double phase = 2.0 * 3.14159265358979323846 * frequency *
        static_cast<double>(index) / 16000.0;
    output.push_back(conditioner.process(
        static_cast<int32_t>(sin(phase) * amplitude)));
  }
  return output;
}

int main() {
  VoiceConditionerMetrics quietVoiceMetrics;
  const std::vector<int32_t> quietVoice = conditionTone(
      1000.0, 48, &quietVoiceMetrics);
  assert(rmsTail(quietVoice) > 100.0);
  assert(quietVoiceMetrics.outputPeak > 0);

  VoiceConditionerMetrics voiceMetrics;
  const std::vector<int32_t> voice = conditionTone(1000.0, 800, &voiceMetrics);
  assert(rmsTail(voice) > 2500.0);
  assert(voiceMetrics.maximumGainQ12 <=
         static_cast<uint32_t>(VoiceConditioner::kMaximumGainQ12));

  VoiceConditionerMetrics hissMetrics;
  const std::vector<int32_t> hiss = conditionTone(6000.0, 800, &hissMetrics);
  assert(rmsTail(hiss) < rmsTail(voice) / 30.0);

  VoiceConditioner cleanConditioner;
  VoiceConditioner mixedConditioner;
  VoiceConditionerMetrics cleanMetrics;
  VoiceConditionerMetrics mixedMetrics;
  cleanConditioner.reset(&cleanMetrics);
  mixedConditioner.reset(&mixedMetrics);
  std::vector<int32_t> cleanSpeech;
  std::vector<int32_t> speechWithHiss;
  std::vector<int32_t> mixedResidual;
  cleanSpeech.reserve(16000);
  speechWithHiss.reserve(16000);
  mixedResidual.reserve(16000);
  for (size_t index = 0; index < 16000; ++index) {
    const double voicePhase = 2.0 * 3.14159265358979323846 * 1000.0 *
        static_cast<double>(index) / 16000.0;
    const double hissPhase = 2.0 * 3.14159265358979323846 * 6000.0 *
        static_cast<double>(index) / 16000.0;
    const int32_t cleanInput = static_cast<int32_t>(sin(voicePhase) * 800);
    const int32_t mixedInput = cleanInput +
        static_cast<int32_t>(sin(hissPhase) * 800);
    cleanSpeech.push_back(cleanConditioner.process(cleanInput));
    speechWithHiss.push_back(mixedConditioner.process(mixedInput));
    mixedResidual.push_back(speechWithHiss.back() - cleanSpeech.back());
  }
  assert(rmsTail(cleanSpeech) > 2500.0);
  assert(rmsTail(mixedResidual) < rmsTail(cleanSpeech) / 30.0);

  VoiceConditionerMetrics noiseMetrics;
  VoiceConditioner conditioner;
  conditioner.reset(&noiseMetrics);
  std::vector<int32_t> noise;
  noise.reserve(16000);
  uint32_t random = 0x9e3779b9u;
  for (size_t index = 0; index < 16000; ++index) {
    random ^= random << 13;
    random ^= random >> 17;
    random ^= random << 5;
    const int32_t sample = static_cast<int32_t>(random % 65) - 32;
    noise.push_back(conditioner.process(sample));
  }
  assert(rmsTail(noise) < 8.0);
  assert(noiseMetrics.suppressedSamples > 15000);
  assert(noiseMetrics.estimatedNoiseFloor >= 8);
  assert(noiseMetrics.estimatedNoiseFloor <= 32);

  std::vector<int32_t> speechAfterNoise;
  speechAfterNoise.reserve(8000);
  for (size_t index = 0; index < 8000; ++index) {
    const double phase = 2.0 * 3.14159265358979323846 * 1000.0 *
        static_cast<double>(index) / 16000.0;
    speechAfterNoise.push_back(conditioner.process(
        static_cast<int32_t>(sin(phase) * 800)));
  }
  assert(rmsTail(speechAfterNoise) > 2500.0);

  VoiceConditionerMetrics limiterMetrics;
  const std::vector<int32_t> loud = conditionTone(1000.0, 30000,
                                                   &limiterMetrics);
  assert(limiterMetrics.limitedSamples > 0);
  assert(limiterMetrics.outputPeak == VoiceConditioner::kLimiter);
  for (const int32_t sample : loud) {
    assert(sample <= VoiceConditioner::kLimiter);
    assert(sample >= -VoiceConditioner::kLimiter);
  }
  return 0;
}
