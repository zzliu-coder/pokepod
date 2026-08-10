#pragma once

#include <stdint.h>

namespace pokepod {

enum class AudioI2sRoute : uint8_t { capture, playback };

struct AudioI2sDataPins {
  int8_t dataOut;
  int8_t dataIn;
};

constexpr AudioI2sDataPins audioI2sDataPins(
    AudioI2sRoute route, int8_t boardDataOut, int8_t boardDataIn) {
  return route == AudioI2sRoute::playback
      ? AudioI2sDataPins{boardDataOut, -1}
      : AudioI2sDataPins{-1, boardDataIn};
}

}  // namespace pokepod
