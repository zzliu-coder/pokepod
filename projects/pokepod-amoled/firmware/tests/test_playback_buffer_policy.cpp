#include <assert.h>

#include "../PokePodAmoled/PlaybackBufferPolicy.h"

using namespace pokepod;

int main() {
  assert(playbackReadSize(4096) == 1024);
  assert(playbackReadSize(100) == 100);
  assert(playbackFeedSize(1024) == 128);
  assert(playbackFeedSize(63) == 62);
  assert(playbackFeedSize(1) == 0);
  return 0;
}
