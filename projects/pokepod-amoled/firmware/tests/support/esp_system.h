#pragma once

#include <cstdint>

inline uint32_t esp_random() {
  static uint32_t value = 0x4f1bbcdcU;
  value ^= value << 13;
  value ^= value >> 17;
  value ^= value << 5;
  return value;
}
