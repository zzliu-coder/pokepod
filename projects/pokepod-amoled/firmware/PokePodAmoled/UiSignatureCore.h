#pragma once

#include <stddef.h>
#include <stdint.h>
#include <type_traits>

namespace pokepod {

class UiSignatureCore {
 public:
  void add(bool value) {
    addByte(1U);
    addByte(value ? 1U : 0U);
  }

  template <typename T>
  typename std::enable_if<std::is_integral<T>::value &&
                              !std::is_same<T, bool>::value,
                          void>::type
  add(T value) {
    addIntegral(value);
  }

  template <typename T>
  typename std::enable_if<std::is_enum<T>::value, void>::type add(T value) {
    using Raw = typename std::underlying_type<T>::type;
    addIntegral(static_cast<Raw>(value));
  }

  void addText(const char *value, size_t length) {
    addByte(0xffU);
    addIntegral(static_cast<uint32_t>(length));
    for (size_t index = 0; index < length; ++index) {
      addByte(static_cast<uint8_t>(value[index]));
    }
  }

  uint64_t value() const { return hash_; }

 private:
  void addByte(uint8_t value) {
    hash_ ^= value;
    hash_ *= 1099511628211ULL;
  }

  template <typename T>
  void addIntegral(T value) {
    addByte(static_cast<uint8_t>(sizeof(T)));
    using Unsigned = typename std::make_unsigned<T>::type;
    Unsigned encoded = static_cast<Unsigned>(value);
    for (size_t index = 0; index < sizeof(T); ++index) {
      addByte(static_cast<uint8_t>(encoded & 0xffU));
      encoded >>= 8;
    }
  }

  uint64_t hash_ = 1469598103934665603ULL;
};

}  // namespace pokepod
