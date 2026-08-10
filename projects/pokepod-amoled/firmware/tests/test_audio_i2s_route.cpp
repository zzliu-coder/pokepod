#include <assert.h>

#include "../PokePodAmoled/AudioI2sRoute.h"

using namespace pokepod;

int main() {
  const AudioI2sDataPins capture =
      audioI2sDataPins(AudioI2sRoute::capture, 41, 42);
  assert(capture.dataOut == -1);
  assert(capture.dataIn == 42);

  const AudioI2sDataPins playback =
      audioI2sDataPins(AudioI2sRoute::playback, 41, 42);
  assert(playback.dataOut == 41);
  assert(playback.dataIn == -1);
  return 0;
}
