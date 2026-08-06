#include <cassert>
#include <cstring>

#include "../PokePodAmoled/LinkFrame.h"

int main() {
  using namespace pokepod;
  const uint8_t known[] = {'1','2','3','4','5','6','7','8','9'};
  assert(linkCrc32(known, sizeof(known)) == 0xcbf43926U);

  const uint8_t payload[] = {'{','}','\n'};
  LinkFrameHeader input;
  input.type = LinkFrameType::requestJson;
  input.flags = 7;
  input.requestId = 0x10203040;
  input.payloadLength = sizeof(payload);
  input.payloadCrc32 = linkCrc32(payload, sizeof(payload));
  uint8_t bytes[kLinkHeaderBytes] = {};
  assert(encodeLinkHeader(input, bytes, sizeof(bytes)));
  LinkFrameHeader decoded;
  assert(decodeLinkHeader(bytes, sizeof(bytes), decoded));
  assert(decoded.version == 2);
  assert(decoded.type == LinkFrameType::requestJson);
  assert(decoded.flags == 7);
  assert(decoded.requestId == 0x10203040);
  assert(validateLinkPayload(decoded, payload, sizeof(payload)));

  bytes[0] = 'X';
  assert(!decodeLinkHeader(bytes, sizeof(bytes), decoded));
  bytes[0] = 'P';
  bytes[4] = 3;
  assert(!decodeLinkHeader(bytes, sizeof(bytes), decoded));
  bytes[4] = 2;
  bytes[5] = static_cast<uint8_t>(LinkFrameType::data);
  linkPut32(bytes + 12, kLinkMaxDataBytes + 1);
  assert(!decodeLinkHeader(bytes, sizeof(bytes), decoded));
  return 0;
}
