#pragma once

#include <cstddef>
#include <cstdlib>
#include <unordered_map>

constexpr unsigned MALLOC_CAP_SPIRAM = 1U << 0;
constexpr unsigned MALLOC_CAP_8BIT = 1U << 1;
constexpr unsigned MALLOC_CAP_INTERNAL = 1U << 2;

namespace fake_heap_caps {

inline std::unordered_map<void *, size_t> &allocations() {
  static std::unordered_map<void *, size_t> values;
  return values;
}

inline size_t liveBytes() {
  size_t total = 0;
  for (const auto &allocation : allocations()) total += allocation.second;
  return total;
}

}  // namespace fake_heap_caps

inline void *heap_caps_calloc(size_t count, size_t size, unsigned) {
  void *value = std::calloc(count, size);
  if (value != nullptr) fake_heap_caps::allocations()[value] = count * size;
  return value;
}

inline void *heap_caps_malloc(size_t size, unsigned) {
  void *value = std::malloc(size);
  if (value != nullptr) fake_heap_caps::allocations()[value] = size;
  return value;
}

inline void heap_caps_free(void *value) {
  if (value == nullptr) return;
  fake_heap_caps::allocations().erase(value);
  std::free(value);
}

inline size_t heap_caps_get_free_size(unsigned) { return 8U * 1024U * 1024U; }
inline size_t heap_caps_get_largest_free_block(unsigned) {
  return 8U * 1024U * 1024U;
}
