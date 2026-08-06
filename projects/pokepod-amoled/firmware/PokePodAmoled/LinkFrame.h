#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace pokepod {

constexpr uint8_t kLinkVersion = 2;
constexpr size_t kLinkHeaderBytes = 20;
constexpr uint32_t kLinkMaxControlBytes = 4096;
constexpr uint32_t kLinkMaxDataBytes = 16384;

enum class LinkFrameType : uint8_t {
  requestJson = 1,
  responseJson = 2,
  data = 3,
  eventJson = 4,
};

struct LinkFrameHeader {
  uint8_t version = kLinkVersion;
  LinkFrameType type = LinkFrameType::requestJson;
  uint16_t flags = 0;
  uint32_t requestId = 0;
  uint32_t payloadLength = 0;
  uint32_t payloadCrc32 = 0;
};

inline uint32_t linkCrc32(const uint8_t *data, size_t size) {
  uint32_t crc = 0xffffffffU;
  for (size_t index = 0; index < size; ++index) {
    crc ^= data[index];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^ (0xedb88320U & static_cast<uint32_t>(-(static_cast<int32_t>(crc & 1U))));
    }
  }
  return ~crc;
}

inline void linkPut16(uint8_t *target, uint16_t value) {
  target[0] = static_cast<uint8_t>(value & 0xff);
  target[1] = static_cast<uint8_t>((value >> 8) & 0xff);
}

inline void linkPut32(uint8_t *target, uint32_t value) {
  target[0] = static_cast<uint8_t>(value & 0xff);
  target[1] = static_cast<uint8_t>((value >> 8) & 0xff);
  target[2] = static_cast<uint8_t>((value >> 16) & 0xff);
  target[3] = static_cast<uint8_t>((value >> 24) & 0xff);
}

inline uint16_t linkGet16(const uint8_t *source) {
  return static_cast<uint16_t>(source[0]) |
         (static_cast<uint16_t>(source[1]) << 8);
}

inline uint32_t linkGet32(const uint8_t *source) {
  return static_cast<uint32_t>(source[0]) |
         (static_cast<uint32_t>(source[1]) << 8) |
         (static_cast<uint32_t>(source[2]) << 16) |
         (static_cast<uint32_t>(source[3]) << 24);
}

inline bool encodeLinkHeader(const LinkFrameHeader &header, uint8_t *encoded,
                             size_t capacity) {
  if (encoded == nullptr || capacity < kLinkHeaderBytes) return false;
  memcpy(encoded, "PPV2", 4);
  encoded[4] = header.version;
  encoded[5] = static_cast<uint8_t>(header.type);
  linkPut16(encoded + 6, header.flags);
  linkPut32(encoded + 8, header.requestId);
  linkPut32(encoded + 12, header.payloadLength);
  linkPut32(encoded + 16, header.payloadCrc32);
  return true;
}

inline bool decodeLinkHeader(const uint8_t *encoded, size_t size,
                             LinkFrameHeader &header) {
  if (encoded == nullptr || size < kLinkHeaderBytes || memcmp(encoded, "PPV2", 4) != 0) return false;
  header.version = encoded[4];
  const uint8_t type = encoded[5];
  if (header.version != kLinkVersion || type < 1 || type > 4) return false;
  header.type = static_cast<LinkFrameType>(type);
  header.flags = linkGet16(encoded + 6);
  header.requestId = linkGet32(encoded + 8);
  header.payloadLength = linkGet32(encoded + 12);
  header.payloadCrc32 = linkGet32(encoded + 16);
  const uint32_t limit = header.type == LinkFrameType::data ? kLinkMaxDataBytes : kLinkMaxControlBytes;
  return header.payloadLength <= limit;
}

inline bool validateLinkPayload(const LinkFrameHeader &header,
                                const uint8_t *payload, size_t size) {
  return size == header.payloadLength &&
         (size == 0 || payload != nullptr) &&
         linkCrc32(payload, size) == header.payloadCrc32;
}

}  // namespace pokepod
