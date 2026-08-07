#pragma once

#include <stddef.h>
#include <stdint.h>

namespace pokepod {

constexpr size_t base64EncodedLength(size_t inputBytes) {
  return ((inputBytes + 2) / 3) * 4;
}

// Keep TLS writes large enough to avoid one encrypted socket operation per
// Base64 quartet. 4096 is divisible by four and remains safe inside the
// dedicated 16 KB ASR task stack.
constexpr size_t kBase64OutputChunkBytes = 4096;

class Base64StreamEncoder {
 public:
  void reset() {
    pendingLength_ = 0;
    outputLength_ = 0;
  }

  template <typename Sink>
  bool append(const uint8_t *data, size_t length, Sink sink) {
    if (data == nullptr && length != 0) return false;
    uint8_t output[4];
    size_t offset = 0;
    while (pendingLength_ < 3 && offset < length) {
      pending_[pendingLength_++] = data[offset++];
    }
    if (pendingLength_ == 3) {
      encodeTriple(pending_, 3, output);
      if (!emit(output, sink)) return false;
      pendingLength_ = 0;
    }
    while (offset + 3 <= length) {
      encodeTriple(data + offset, 3, output);
      if (!emit(output, sink)) return false;
      offset += 3;
    }
    while (offset < length) pending_[pendingLength_++] = data[offset++];
    return true;
  }

  template <typename Sink>
  bool finish(Sink sink) {
    if (pendingLength_ != 0) {
      uint8_t output[4];
      encodeTriple(pending_, pendingLength_, output);
      pendingLength_ = 0;
      if (!emit(output, sink)) return false;
    }
    if (outputLength_ == 0) return true;
    const bool ok = sink(output_, outputLength_);
    outputLength_ = 0;
    return ok;
  }

 private:
  template <typename Sink>
  bool emit(const uint8_t output[4], Sink sink) {
    for (size_t index = 0; index < 4; ++index) {
      output_[outputLength_++] = output[index];
    }
    if (outputLength_ < sizeof(output_)) return true;
    const bool ok = sink(output_, outputLength_);
    outputLength_ = 0;
    return ok;
  }

  static void encodeTriple(const uint8_t *input, size_t length, uint8_t output[4]) {
    static constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const uint32_t value = static_cast<uint32_t>(input[0]) << 16 |
        (length > 1 ? static_cast<uint32_t>(input[1]) << 8 : 0) |
        (length > 2 ? input[2] : 0);
    output[0] = alphabet[(value >> 18) & 0x3f];
    output[1] = alphabet[(value >> 12) & 0x3f];
    output[2] = length > 1 ? alphabet[(value >> 6) & 0x3f] : '=';
    output[3] = length > 2 ? alphabet[value & 0x3f] : '=';
  }

  uint8_t pending_[3] = {};
  size_t pendingLength_ = 0;
  uint8_t output_[kBase64OutputChunkBytes] = {};
  size_t outputLength_ = 0;
};

}  // namespace pokepod
