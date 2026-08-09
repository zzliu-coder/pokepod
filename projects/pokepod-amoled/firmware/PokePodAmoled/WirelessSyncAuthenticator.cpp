#include "WirelessSyncAuthenticator.h"

#include <cJSON.h>
#include <esp_random.h>
#include <mbedtls/md.h>

#include "WirelessSyncIdentity.h"

namespace pokepod {
namespace {

const char *jsonString(cJSON *root, const char *name) {
  cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
  return cJSON_IsString(item) && item->valuestring != nullptr
      ? item->valuestring : nullptr;
}

int64_t jsonInteger(cJSON *root, const char *name) {
  cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
  return cJSON_IsNumber(item) ? static_cast<int64_t>(item->valuedouble) : -1;
}

String printJson(cJSON *root) {
  char *printed = cJSON_PrintUnformatted(root);
  const String value = printed == nullptr ? String() : String(printed);
  cJSON_free(printed);
  return value;
}

bool hmacSha256(const uint8_t secret[32], const std::string &message,
                uint8_t output[32]) {
  const mbedtls_md_info_t *info =
      mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  return info != nullptr && mbedtls_md_hmac(
      info, secret, 32,
      reinterpret_cast<const uint8_t *>(message.data()), message.size(),
      output) == 0;
}

}  // namespace

void WirelessSyncAuthenticator::begin(WirelessSyncIdentity &identity,
                                      WirelessReplayGuard &replay,
                                      Print &log) {
  identity_ = &identity;
  replay_ = &replay;
  log_ = &log;
  reset();
}

void WirelessSyncAuthenticator::reset() {
  policy_.reset();
  completed_.clear();
  receivePhase_ = ReceivePhase::magic;
  headerUsed_ = payloadUsed_ = magicMatched_ = 0;
  currentHeader_ = LinkFrameHeader();
}

void WirelessSyncAuthenticator::poll(Stream &stream) {
  size_t budget = 32768;
  while (!authenticated() && !rejected() && stream.available() > 0 &&
         budget-- > 0) {
    const int value = stream.read();
    if (value >= 0) consume(stream, static_cast<uint8_t>(value));
  }
}

void WirelessSyncAuthenticator::consume(Stream &stream, uint8_t value) {
  if (receivePhase_ == ReceivePhase::magic) {
    static constexpr uint8_t magic[4] = {'P', 'P', 'V', '2'};
    if (value == magic[magicMatched_]) {
      header_[magicMatched_++] = value;
      if (magicMatched_ == 4) {
        headerUsed_ = 4;
        receivePhase_ = ReceivePhase::header;
      }
    } else {
      magicMatched_ = value == 'P' ? 1 : 0;
      if (magicMatched_ == 1) header_[0] = value;
    }
    return;
  }
  if (receivePhase_ == ReceivePhase::header) {
    header_[headerUsed_++] = value;
    if (headerUsed_ < sizeof(header_)) return;
    if (!decodeLinkHeader(header_, sizeof(header_), currentHeader_) ||
        currentHeader_.type != LinkFrameType::requestJson ||
        currentHeader_.payloadLength > sizeof(payload_)) {
      sendError(stream, linkGet32(header_ + 8), "unsupported-version",
                "invalid authenticated Link v2 header");
      policy_.reject();
      return;
    }
    payloadUsed_ = 0;
    receivePhase_ = ReceivePhase::payload;
    if (currentHeader_.payloadLength == 0) process(stream);
    return;
  }
  payload_[payloadUsed_++] = value;
  if (payloadUsed_ == currentHeader_.payloadLength) process(stream);
}

void WirelessSyncAuthenticator::process(Stream &stream) {
  const LinkFrameHeader header = currentHeader_;
  const size_t size = payloadUsed_;
  receivePhase_ = ReceivePhase::magic;
  headerUsed_ = payloadUsed_ = magicMatched_ = 0;
  currentHeader_ = LinkFrameHeader();
  if (header.requestId == 0 || completed_.contains(header.requestId) ||
      !validateLinkPayload(header, payload_, size)) {
    sendError(stream, header.requestId, "authentication-failed",
              "invalid or duplicate authentication frame");
    policy_.reject();
    return;
  }
  cJSON *root = cJSON_ParseWithLength(
      reinterpret_cast<const char *>(payload_), size);
  const char *operation = root == nullptr ? nullptr :
      jsonString(root, "operation");
  if (root == nullptr || !cJSON_IsObject(root) || operation == nullptr) {
    cJSON_Delete(root);
    sendError(stream, header.requestId, "authentication-failed",
              "malformed authentication request");
    policy_.reject();
    return;
  }
  if (strcmp(operation, "hello") == 0) {
    handleHello(stream, header.requestId, root);
  } else if (strcmp(operation, "auth") == 0) {
    handleAuth(stream, header.requestId, root);
  } else {
    sendError(stream, header.requestId, "authentication-failed",
              "authenticate before Link v2 operations");
    policy_.reject();
  }
  cJSON_Delete(root);
  completed_.complete(header.requestId);
}

void WirelessSyncAuthenticator::handleHello(Stream &stream,
                                             uint32_t requestId, void *value) {
  cJSON *root = static_cast<cJSON *>(value);
  if (identity_ == nullptr || replay_ == nullptr || !identity_->ready() ||
      !identity_->paired()) {
    sendError(stream, requestId, "not-paired",
              "wireless sync requires USB pairing");
    policy_.reject();
    return;
  }
  const char *kind = jsonString(root, "kind");
  const char *pairingId = jsonString(root, "pairingId");
  const char *clientNonce = jsonString(root, "clientNonce");
  const char *secureTransport = jsonString(root, "secureTransport");
  if (kind == nullptr || strcmp(kind, "client-hello") != 0 ||
      secureTransport == nullptr || strcmp(secureTransport, "tls") != 0 ||
      jsonInteger(root, "schemaVersion") != 1 ||
      jsonInteger(root, "version") != 2 || pairingId == nullptr ||
      strcmp(pairingId, identity_->pairingId()) != 0 ||
      clientNonce == nullptr ||
      !validWirelessNonce(clientNonce)) {
    sendError(stream, requestId,
              pairingId == nullptr ||
                      strcmp(pairingId, identity_->pairingId()) != 0
                  ? "not-paired" : "authentication-failed",
              "wireless hello does not match the paired device");
    policy_.reject();
    return;
  }
  uint8_t serverNonceBytes[16];
  esp_fill_random(serverNonceBytes, sizeof(serverNonceBytes));
  const std::string serverNonce = base64UrlEncode(
      serverNonceBytes, sizeof(serverNonceBytes));
  if (!policy_.acceptHello(1, 2, pairingId, identity_->pairingId(),
                           clientNonce, serverNonce, *replay_)) {
    sendError(stream, requestId, "replay",
              "wireless client nonce was already used");
    return;
  }
  uint8_t proof[32];
  if (!hmacSha256(identity_->secret(), wirelessProofMessage(
          "server", pairingId, clientNonce, serverNonce), proof)) {
    sendError(stream, requestId, "authentication-failed",
              "server proof failed");
    policy_.reject();
    return;
  }
  cJSON *response = cJSON_CreateObject();
  cJSON_AddStringToObject(response, "status", "ok");
  cJSON_AddNumberToObject(response, "version", 2);
  cJSON_AddNumberToObject(response, "schemaVersion", 1);
  cJSON_AddStringToObject(response, "kind", "server-challenge");
  cJSON_AddStringToObject(response, "serverNonce", serverNonce.c_str());
  const std::string encoded = base64UrlEncode(proof, sizeof(proof));
  cJSON_AddStringToObject(response, "serverProof", encoded.c_str());
  cJSON *capabilities = cJSON_AddArrayToObject(response, "capabilities");
  for (const char *capability : {"identity", "fingerprint", "read",
       "stage-write", "commit", "command", "result"}) {
    cJSON_AddItemToArray(capabilities, cJSON_CreateString(capability));
  }
  sendJson(stream, requestId, printJson(response));
  cJSON_Delete(response);
}

void WirelessSyncAuthenticator::handleAuth(Stream &stream,
                                            uint32_t requestId, void *value) {
  cJSON *root = static_cast<cJSON *>(value);
  const char *kind = jsonString(root, "kind");
  const char *pairingId = jsonString(root, "pairingId");
  const char *clientNonce = jsonString(root, "clientNonce");
  const char *serverNonce = jsonString(root, "serverNonce");
  const char *clientProof = jsonString(root, "clientProof");
  const char *secureTransport = jsonString(root, "secureTransport");
  if (identity_ == nullptr || kind == nullptr ||
      strcmp(kind, "client-auth") != 0 || secureTransport == nullptr ||
      strcmp(secureTransport, "tls") != 0 || pairingId == nullptr ||
      clientNonce == nullptr || serverNonce == nullptr ||
      clientProof == nullptr || jsonInteger(root, "schemaVersion") != 1 ||
      jsonInteger(root, "version") != 2 ||
      !policy_.acceptsClientProof(1, 2, pairingId, clientNonce, serverNonce)) {
    sendError(stream, requestId, "authentication-failed",
              "wireless client proof context is invalid");
    policy_.reject();
    return;
  }
  std::vector<uint8_t> decodedProof;
  uint8_t expected[32];
  const bool valid = base64UrlDecode(clientProof, decodedProof) &&
      decodedProof.size() == sizeof(expected) &&
      hmacSha256(identity_->secret(), wirelessProofMessage(
          "client", pairingId, clientNonce, serverNonce), expected) &&
      constantTimeEqual(decodedProof.data(), expected, sizeof(expected));
  if (!valid) {
    sendError(stream, requestId, "authentication-failed",
              "wireless client proof does not match");
    policy_.reject();
    return;
  }
  policy_.authenticated();
  sendJson(stream, requestId,
           "{\"status\":\"ok\",\"version\":2,\"authenticated\":true}");
  if (log_ != nullptr) {
    log_->println("{\"event\":\"wifi_sync_auth\",\"ok\":true}");
  }
}

bool WirelessSyncAuthenticator::sendJson(Stream &stream, uint32_t requestId,
                                         const String &json) {
  LinkFrameHeader header;
  header.type = LinkFrameType::responseJson;
  header.requestId = requestId;
  header.payloadLength = json.length();
  header.payloadCrc32 = linkCrc32(
      reinterpret_cast<const uint8_t *>(json.c_str()), json.length());
  uint8_t encoded[kLinkHeaderBytes];
  if (!encodeLinkHeader(header, encoded, sizeof(encoded)) ||
      stream.write(encoded, sizeof(encoded)) != sizeof(encoded)) {
    return false;
  }
  return json.isEmpty() || stream.write(
      reinterpret_cast<const uint8_t *>(json.c_str()), json.length()) ==
          json.length();
}

void WirelessSyncAuthenticator::sendError(Stream &stream, uint32_t requestId,
                                          const char *code,
                                          const char *message) {
  cJSON *root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "status", "error");
  cJSON_AddNumberToObject(root, "version", 2);
  cJSON_AddNumberToObject(root, "schemaVersion", 1);
  cJSON_AddStringToObject(root, "kind", "error");
  cJSON_AddStringToObject(root, "code", code);
  cJSON_AddStringToObject(root, "message", message);
  sendJson(stream, requestId, printJson(root));
  cJSON_Delete(root);
  if (log_ != nullptr) {
    log_->printf("{\"event\":\"wifi_sync_auth\",\"ok\":false,\"code\":\"%s\"}\n",
                 code);
  }
}

}  // namespace pokepod
