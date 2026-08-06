#pragma once

#include <stddef.h>
#include <stdint.h>

namespace pokepod {

inline uint32_t decodeUtf8(const char *text, size_t length, size_t &offset) {
  if (text == nullptr || offset >= length) return 0;
  const uint8_t first = static_cast<uint8_t>(text[offset++]);
  if (first < 0x80) return first;
  uint32_t value = 0;
  size_t continuation = 0;
  if ((first & 0xe0) == 0xc0) {
    value = first & 0x1f;
    continuation = 1;
  } else if ((first & 0xf0) == 0xe0) {
    value = first & 0x0f;
    continuation = 2;
  } else if ((first & 0xf8) == 0xf0) {
    value = first & 0x07;
    continuation = 3;
  } else {
    return 0xfffd;
  }
  if (offset + continuation > length) {
    offset = length;
    return 0xfffd;
  }
  for (size_t index = 0; index < continuation; ++index) {
    const uint8_t next = static_cast<uint8_t>(text[offset++]);
    if ((next & 0xc0) != 0x80) return 0xfffd;
    value = (value << 6) | (next & 0x3f);
  }
  return value;
}

}  // namespace pokepod
