#include <assert.h>
#include <math.h>
#include <stdint.h>

#include <vector>

#include "AudioFrontEnd.h"

using namespace pokepod;

static void appendFrame(std::vector<uint8_t> &stereo, int16_t left,
                        int16_t right) {
  stereo.push_back(static_cast<uint8_t>(left & 0xff));
  stereo.push_back(static_cast<uint8_t>((left >> 8) & 0xff));
  stereo.push_back(static_cast<uint8_t>(right & 0xff));
  stereo.push_back(static_cast<uint8_t>((right >> 8) & 0xff));
}

static std::vector<int16_t> processTone(double frequency, int16_t amplitude,
                                        bool rightOnly = false) {
  AudioFrontEnd frontEnd;
  frontEnd.reset();
  std::vector<int16_t> output;
  for (size_t base = 0; base < 4800; base += 48) {
    std::vector<uint8_t> stereo;
    stereo.reserve(192);
    for (size_t offset = 0; offset < 48; ++offset) {
      const double phase = 2.0 * 3.14159265358979323846 * frequency *
          static_cast<double>(base + offset) / 48000.0;
      const int16_t sample = static_cast<int16_t>(sin(phase) * amplitude);
      appendFrame(stereo, rightOnly ? 0 : sample,
                  rightOnly ? sample : sample);
    }
    uint8_t mono[192] = {};
    const size_t bytes = frontEnd.processStereo16(
        stereo.data(), stereo.size(), mono, sizeof(mono));
    for (size_t index = 0; index + 1 < bytes; index += 2) {
      output.push_back(static_cast<int16_t>(
          static_cast<uint16_t>(mono[index]) |
          static_cast<uint16_t>(mono[index + 1]) << 8));
    }
  }
  if (rightOnly) {
    assert(frontEnd.selectedChannel() == AudioInputChannel::right);
  } else {
    assert(frontEnd.selectedChannel() == AudioInputChannel::left);
  }
  assert(frontEnd.metrics().outputPeak <= AudioFrontEnd::kLimiter);
  return output;
}

static double rmsTail(const std::vector<int16_t> &samples) {
  const size_t first = samples.size() / 2;
  double energy = 0.0;
  for (size_t index = first; index < samples.size(); ++index) {
    energy += static_cast<double>(samples[index]) * samples[index];
  }
  return sqrt(energy / static_cast<double>(samples.size() - first));
}

int main() {
  AudioFrontEnd v1Profile;
  v1Profile.configure(AudioDspProfile::v1Measured);
  v1Profile.reset();
  assert(v1Profile.metrics().profile == AudioDspProfile::v1Measured);
  AudioFrontEnd v2Profile;
  v2Profile.configure(AudioDspProfile::v2Baseline);
  v2Profile.reset();
  assert(v2Profile.metrics().profile == AudioDspProfile::v2Baseline);

  const std::vector<int16_t> voice = processTone(1000.0, 500, true);
  assert(voice.size() > 1500);
  assert(rmsTail(voice) > 1500.0);

  const std::vector<int16_t> quiet = processTone(1000.0, 8);
  assert(rmsTail(quiet) < 80.0);

  // The V1 microphone can produce weak post-decimation speech.  It must stay
  // audible instead of being flattened into an all-zero capsule.
  const std::vector<int16_t> weakVoice = processTone(1000.0, 48);
  assert(rmsTail(weakVoice) > 100.0);

  const std::vector<int16_t> passband = processTone(1000.0, 4000);
  const std::vector<int16_t> voiceStopband = processTone(6000.0, 4000);
  assert(rmsTail(voiceStopband) < rmsTail(passband) / 30.0);

  const std::vector<int16_t> stopband = processTone(12000.0, 4000);
  assert(rmsTail(stopband) < rmsTail(passband) / 20.0);

  AudioFrontEnd frontEnd;
  frontEnd.reset();
  assert(frontEnd.processStereo16(nullptr, 0, nullptr, 0) == 0);
  for (size_t base = 0; base < 2400; base += 48) {
    std::vector<uint8_t> stereo;
    for (size_t offset = 0; offset < 48; ++offset) {
      const int16_t sample = ((base + offset) / 12) % 2 == 0
          ? 32767 : -32767;
      appendFrame(stereo, sample, sample);
    }
    uint8_t mono[192] = {};
    (void)frontEnd.processStereo16(stereo.data(), stereo.size(),
                                   mono, sizeof(mono));
  }
  assert(frontEnd.metrics().clippedInputSamples > 0);
  assert(frontEnd.metrics().limitedSamples > 0);
  assert(frontEnd.metrics().outputPeak == AudioFrontEnd::kLimiter);
  return 0;
}
