#include <cassert>
#include <cstring>

#include "FirmwareUpdatePolicy.h"

using namespace pokepod;

int main() {
  constexpr const char *kSha =
      "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
  assert(FirmwareUpdatePolicy::validSha256(kSha));
  assert(!FirmwareUpdatePolicy::validSha256(nullptr));
  char upper[65] = {};
  std::strcpy(upper, kSha);
  upper[0] = 'A';
  assert(FirmwareUpdatePolicy::validSha256(upper));
  upper[64] = '0';
  assert(!FirmwareUpdatePolicy::validSha256(upper));
  assert(!FirmwareUpdatePolicy::validImageSize(1023));
  assert(FirmwareUpdatePolicy::validImageSize(1024));
  assert(FirmwareUpdatePolicy::validImageSize(0x300000));
  assert(!FirmwareUpdatePolicy::validImageSize(0x300001));
  assert(FirmwareUpdatePolicy::acceptsChunk(0, 1024, 128));
  assert(FirmwareUpdatePolicy::acceptsChunk(896, 1024, 128));
  assert(!FirmwareUpdatePolicy::acceptsChunk(1024, 1024, 1));
  assert(!FirmwareUpdatePolicy::acceptsChunk(900, 1024, 125));
  assert(FirmwareUpdatePolicy::complete(1024, 1024));
  assert(!FirmwareUpdatePolicy::complete(1023, 1024));
  return 0;
}
