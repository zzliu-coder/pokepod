#include <cassert>
#include <cstring>

#include "../PokePodAmoled/Utf8Policy.h"

int main() {
  const char *text = "A语音";
  size_t offset = 0;
  assert(pokepod::decodeUtf8(text, std::strlen(text), offset) == 'A');
  assert(pokepod::decodeUtf8(text, std::strlen(text), offset) == 0x8bed);
  assert(pokepod::decodeUtf8(text, std::strlen(text), offset) == 0x97f3);
  assert(offset == std::strlen(text));
  return 0;
}
