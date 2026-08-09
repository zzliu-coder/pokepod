#pragma once

#include <Arduino.h>

#include "LinkFrame.h"
#include "LinkPolicy.h"
#include "WirelessSyncProtocol.h"

namespace pokepod {

class WirelessSyncIdentity;

class WirelessSyncAuthenticator {
 public:
  void begin(WirelessSyncIdentity &identity, WirelessReplayGuard &replay,
             Print &log);
  void reset();
  void poll(Stream &stream);
  bool authenticated() const {
    return policy_.phase() == WirelessAuthPhase::authenticated;
  }
  bool rejected() const {
    return policy_.phase() == WirelessAuthPhase::rejected;
  }

 private:
  enum class ReceivePhase : uint8_t { magic, header, payload };

  void consume(Stream &stream, uint8_t value);
  void process(Stream &stream);
  void handleHello(Stream &stream, uint32_t requestId, void *root);
  void handleAuth(Stream &stream, uint32_t requestId, void *root);
  bool sendJson(Stream &stream, uint32_t requestId, const String &json);
  void sendError(Stream &stream, uint32_t requestId, const char *code,
                 const char *message);

  WirelessSyncIdentity *identity_ = nullptr;
  WirelessReplayGuard *replay_ = nullptr;
  Print *log_ = nullptr;
  WirelessAuthPolicy policy_;
  LinkRequestHistory completed_;
  ReceivePhase receivePhase_ = ReceivePhase::magic;
  uint8_t header_[kLinkHeaderBytes] = {};
  size_t headerUsed_ = 0;
  uint8_t magicMatched_ = 0;
  LinkFrameHeader currentHeader_;
  uint8_t payload_[kLinkMaxControlBytes] = {};
  size_t payloadUsed_ = 0;
};

}  // namespace pokepod
