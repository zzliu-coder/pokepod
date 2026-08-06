#pragma once

namespace pokepod {

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
