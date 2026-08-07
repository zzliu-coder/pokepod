#include <cassert>
#include <cstring>

#include "../PokePodAmoled/CapsulePolicy.h"

int main() {
  using namespace pokepod;
  uint8_t bytes[16] = {};
  char uuid[37];
  formatUuidV4(bytes, uuid);
  assert(std::strcmp(uuid, "00000000-0000-4000-8000-000000000000") == 0);
  assert(isUuid(uuid));
  assert(isUuid("0d95b7c1-7ce9-4a91-aea2-b64707a05c9f"));
  assert(!isUuid("0d95b7c1-7ce9-4a91-aea2-b64707a05c9"));
  assert(!isUuid("0d95b7c1/7ce9-4a91-aea2-b64707a05c9f"));

  assert(safeCapsuleFileName("audio.wav"));
  assert(!safeCapsuleFileName("../audio.wav"));
  assert(!safeCapsuleFileName("folder/audio.wav"));

  assert(capsuleStatusNeedsStartupRequeue("transcribing"));
  assert(!capsuleStatusNeedsStartupRequeue("queued"));
  assert(!capsuleStatusNeedsStartupRequeue("transcribing-old"));
  assert(!capsuleStatusNeedsStartupRequeue(nullptr));
  assert(!safeCapsuleFileName(""));
  assert(std::strcmp(kCapsuleWavFile, "audio.wav") == 0);
  assert(std::strcmp(kCapsuleWavFormat, "wav-pcm-s16le") == 0);
  return 0;
}
