#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#include <array>

#include "FixedChineseFont.h"

using namespace pokepod;

namespace {

template <size_t EncodedBytes, size_t DecodedBytes>
void expectDecode(const std::array<uint8_t, EncodedBytes> &encoded,
                  const std::array<uint8_t, DecodedBytes> &expected) {
  std::array<uint8_t, DecodedBytes + 2> guarded{};
  guarded.front() = 0xa5;
  guarded.back() = 0x5a;
  assert(decodeFixedGlyphRle(encoded.data(), encoded.size(),
                             guarded.data() + 1, expected.size()));
  for (size_t index = 0; index < expected.size(); ++index) {
    assert(guarded[index + 1] == expected[index]);
  }
  assert(guarded.front() == 0xa5);
  assert(guarded.back() == 0x5a);
}

template <size_t EncodedBytes>
void expectRejected(const std::array<uint8_t, EncodedBytes> &encoded,
                    size_t decodedBytes) {
  std::array<uint8_t, 12> guarded{};
  guarded.front() = 0xa5;
  guarded.back() = 0x5a;
  assert(decodedBytes <= guarded.size() - 2);
  assert(!decodeFixedGlyphRle(encoded.data(), encoded.size(),
                              guarded.data() + 1, decodedBytes));
  assert(guarded.front() == 0xa5);
  assert(guarded.back() == 0x5a);
}

}  // namespace

int main() {
  static_assert(sizeof(FixedGlyphIndex) == 12,
                "compressed index must stay compact");
  expectDecode(std::array<uint8_t, 8>{0x02, 0x10, 0x20, 0x30,
                                      0x82, 0x44, 0x80, 0x80},
               std::array<uint8_t, 7>{0x10, 0x20, 0x30, 0x44,
                                      0x44, 0x44, 0x80});

  // Truncated repeat and literal packets, decoded overflow, and decoded
  // underflow must all fail without writing outside the caller's buffer.
  expectRejected(std::array<uint8_t, 1>{0x80}, 1);
  expectRejected(std::array<uint8_t, 2>{0x02, 0x11}, 3);
  expectRejected(std::array<uint8_t, 2>{0x83, 0x11}, 3);
  expectRejected(std::array<uint8_t, 2>{0x80, 0x11}, 2);
  assert(!decodeFixedGlyphRle(nullptr, 0, nullptr, 0));

  // Exercise real first/last records so generated offsets and the decoder are
  // compiled together, rather than testing a duplicate implementation.
  std::array<uint8_t, 128> compact{};
  const FixedGlyphIndex &space = kFixedGlyphs16[0];
  assert(decodeFixedGlyphRle(kFixedGlyphData16 + space.dataOffset,
                             space.dataLength, compact.data(), compact.size()));
  for (uint8_t value : compact) assert(value == 0);

  std::array<uint8_t, 648> timer{};
  const FixedGlyphIndex &last = kFixedGlyphs36[kFixedGlyphs36Count - 1];
  assert(last.dataOffset + last.dataLength <= kFixedGlyphData36Bytes);
  assert(decodeFixedGlyphRle(kFixedGlyphData36 + last.dataOffset,
                             last.dataLength, timer.data(), timer.size()));
  return 0;
}
