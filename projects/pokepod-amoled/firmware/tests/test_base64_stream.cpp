#include <cassert>
#include <cstdint>
#include <string>

#include "../PokePodAmoled/Base64Stream.h"

int main() {
  using namespace pokepod;
  assert(base64EncodedLength(0) == 0);
  assert(base64EncodedLength(1) == 4);
  assert(base64EncodedLength(3) == 4);
  assert(base64EncodedLength(4) == 8);
  assert(base64EncodedLength(44 + 58500 * 32) == 2496060);

  const std::string input = "PokePod stream test";
  std::string output;
  auto sink = [&output](const uint8_t *data, size_t length) {
    output.append(reinterpret_cast<const char *>(data), length);
    return true;
  };
  Base64StreamEncoder encoder;
  assert(encoder.append(reinterpret_cast<const uint8_t *>(input.data()), 1, sink));
  assert(encoder.append(reinterpret_cast<const uint8_t *>(input.data() + 1), 4, sink));
  assert(encoder.append(reinterpret_cast<const uint8_t *>(input.data() + 5),
                        input.size() - 5, sink));
  assert(encoder.finish(sink));
  assert(output == "UG9rZVBvZCBzdHJlYW0gdGVzdA==");
  return 0;
}
