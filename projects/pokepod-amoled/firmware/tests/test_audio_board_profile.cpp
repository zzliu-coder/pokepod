#include <assert.h>

#include "AudioBoardProfile.h"

using namespace pokepod;

int main() {
  const AudioBoardProfile v1 =
      audioBoardProfile(BoardVariant::v1Sh8601Ft3168);
  const AudioBoardProfile v2 =
      audioBoardProfile(BoardVariant::v2Co5300Cst820);
  assert(v1.valid());
  assert(v2.valid());
  assert(v1.dsp == AudioDspProfile::v1Measured);
  assert(v2.dsp == AudioDspProfile::v2Baseline);
  assert(v1.microphoneGainDb == 30);
  assert(v2.microphoneGainDb == 30);
  assert(!audioBoardProfile(BoardVariant::unknown).valid());

  const AudioCodecPolicy capture = audioCodecPolicy(AudioCodecPath::capture);
  assert(capture.configureMicrophone);
  assert(!capture.configureOutput);
  assert(capture.muteOutput);

  const AudioCodecPolicy playback = audioCodecPolicy(AudioCodecPath::playback);
  assert(!playback.configureMicrophone);
  assert(playback.configureOutput);
  assert(!playback.muteOutput);
  return 0;
}
