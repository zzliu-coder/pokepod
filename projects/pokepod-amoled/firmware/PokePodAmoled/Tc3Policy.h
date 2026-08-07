#pragma once

#include <stddef.h>
#include <stdint.h>

namespace pokepod {

constexpr uint32_t tencentUploadDeadlineMs(size_t encodedBytes) {
  constexpr size_t conservativeBytesPerSecond = 24 * 1024;
  const size_t transferSeconds =
      (encodedBytes + conservativeBytesPerSecond - 1) /
      conservativeBytesPerSecond;
  const size_t estimatedMs = transferSeconds * 1000 + 5000;
  if (estimatedMs < 20000) return 20000;
  return estimatedMs > 120000 ? 120000 : static_cast<uint32_t>(estimatedMs);
}

template <typename Text>
Text tc3CanonicalRequest(const char *method, const char *uri,
                         const char *query, const char *contentType,
                         const char *host, const char *actionLower,
                         const Text &payloadHashHex) {
  Text value(method);
  value += "\n";
  value += uri;
  value += "\n";
  value += query;
  value += "\ncontent-type:";
  value += contentType;
  value += "\nhost:";
  value += host;
  value += "\nx-tc-action:";
  value += actionLower;
  value += "\n\ncontent-type;host;x-tc-action\n";
  value += payloadHashHex;
  return value;
}

template <typename Text>
Text tc3CredentialScope(const char *date, const char *service) {
  Text value(date);
  value += "/";
  value += service;
  value += "/tc3_request";
  return value;
}

template <typename Text>
Text tc3StringToSign(const char *timestamp, const Text &credentialScope,
                     const Text &canonicalHashHex) {
  Text value("TC3-HMAC-SHA256\n");
  value += timestamp;
  value += "\n";
  value += credentialScope;
  value += "\n";
  value += canonicalHashHex;
  return value;
}

}  // namespace pokepod
