#include <cassert>

#include "../PokePodAmoled/FontPolicy.h"

int main() {
  using namespace pokepod;
  const uint32_t glyphs = 21000;
  const uint64_t complete = kFontHeaderBytes +
      static_cast<uint64_t>(glyphs) * kFontEntryBytes;
  assert(validFontLayout(16, 16, glyphs, kFontEntryBytes, complete));
  assert(!validFontLayout(16, 16, glyphs, kFontEntryBytes, complete - 1));
  assert(!validFontLayout(16, 16, glyphs, kFontEntryBytes, complete + 1));
  assert(!validFontLayout(16, 16, 0, kFontEntryBytes, kFontHeaderBytes));
  assert(!validFontLayout(16, 16, kMaximumFontGlyphs + 1,
                          kFontEntryBytes, complete));
  assert(!validFontLayout(24, 16, glyphs, kFontEntryBytes, complete));
  assert(!validFontLayout(16, 16, glyphs, 40, complete));
  return 0;
}
