#pragma once

#include <stdint.h>

#include "FirmwarePolicy.h"

namespace pokepod {

enum class AudioDspProfile : uint8_t {
  unavailable = 0,
  v1Measured,
  v2Baseline,
};

struct AudioBoardProfile {
  AudioDspProfile dsp = AudioDspProfile::unavailable;
  uint8_t microphoneGainDb = 0;
  uint16_t rightChannelEnergyRatioQ8 = 512;  // 2.0x.
  uint16_t channelSelectionMinimumPeak = 32;

  constexpr bool valid() const {
    return dsp != AudioDspProfile::unavailable;
  }
};

constexpr AudioBoardProfile audioBoardProfile(BoardVariant variant) {
  switch (variant) {
    case BoardVariant::v1Sh8601Ft3168:
      return {AudioDspProfile::v1Measured, 30, 512, 32};
    case BoardVariant::v2Co5300Cst820:
      // V2 uses the same ES8311 baseline today, while retaining a distinct
      // profile identity so future measurements cannot silently retune V1.
      return {AudioDspProfile::v2Baseline, 30, 512, 32};
    case BoardVariant::unknown:
      return {};
  }
  return {};
}

inline const char *audioDspProfileName(AudioDspProfile profile) {
  switch (profile) {
    case AudioDspProfile::v1Measured: return "v1_measured";
    case AudioDspProfile::v2Baseline: return "v2_baseline";
    case AudioDspProfile::unavailable: return "unavailable";
  }
  return "unavailable";
}

enum class AudioCodecPath : uint8_t { capture, playback };

struct AudioCodecPolicy {
  bool configureMicrophone = false;
  bool configureOutput = false;
  bool muteOutput = true;
};

constexpr AudioCodecPolicy audioCodecPolicy(AudioCodecPath path) {
  return path == AudioCodecPath::capture
      ? AudioCodecPolicy{true, false, true}
      : AudioCodecPolicy{false, true, false};
}

}  // namespace pokepod
