#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string>
#include <vector>

namespace pokepod {

constexpr uint8_t kWirelessSyncSchemaVersion = 1;
constexpr uint8_t kWirelessSyncLinkVersion = 2;
constexpr size_t kWirelessSyncSecretBytes = 32;
constexpr size_t kWirelessSyncNonceMinimumBytes = 16;
constexpr size_t kWirelessSyncReplayLimit = 64;
constexpr char kWirelessSyncProtocolLabel[] = "pokecapsule-v1";
constexpr char kWirelessSyncServiceType[] = "_pokecapsule._tcp";
constexpr uint16_t kWirelessSyncPort = 57431;

inline bool base64UrlDecode(const std::string &value,
                            std::vector<uint8_t> &output) {
  output.clear();
  if (value.empty() || value.find('=') != std::string::npos ||
      value.size() % 4 == 1) {
    return false;
  }
  uint32_t accumulator = 0;
  uint8_t bits = 0;
  for (const unsigned char character : value) {
    int8_t decoded = -1;
    if (character >= 'A' && character <= 'Z') decoded = character - 'A';
    else if (character >= 'a' && character <= 'z') decoded = character - 'a' + 26;
    else if (character >= '0' && character <= '9') decoded = character - '0' + 52;
    else if (character == '-') decoded = 62;
    else if (character == '_') decoded = 63;
    if (decoded < 0) return false;
    accumulator = (accumulator << 6) |
        static_cast<uint8_t>(decoded);
    bits = static_cast<uint8_t>(bits + 6);
    if (bits >= 8) {
      bits = static_cast<uint8_t>(bits - 8);
      output.push_back(static_cast<uint8_t>(accumulator >> bits));
      accumulator &= (1U << bits) - 1U;
    }
  }
  return bits == 0 || accumulator == 0;
}

inline std::string base64UrlEncode(const uint8_t *bytes, size_t size) {
  static constexpr char alphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  std::string output;
  output.reserve((size * 4 + 2) / 3);
  uint32_t accumulator = 0;
  uint8_t bits = 0;
  for (size_t index = 0; index < size; ++index) {
    accumulator = (accumulator << 8) | bytes[index];
    bits = static_cast<uint8_t>(bits + 8);
    while (bits >= 6) {
      bits = static_cast<uint8_t>(bits - 6);
      output.push_back(alphabet[(accumulator >> bits) & 0x3f]);
    }
  }
  if (bits > 0) output.push_back(alphabet[(accumulator << (6 - bits)) & 0x3f]);
  return output;
}

inline bool validWirelessNonce(const std::string &value) {
  std::vector<uint8_t> decoded;
  return base64UrlDecode(value, decoded) &&
      decoded.size() >= kWirelessSyncNonceMinimumBytes;
}

inline std::string wirelessProofMessage(const char *role,
                                        const std::string &pairingId,
                                        const std::string &clientNonce,
                                        const std::string &serverNonce) {
  return std::string(kWirelessSyncProtocolLabel) + "|" + role + "|" +
      pairingId + "|" + clientNonce + "|" + serverNonce;
}

inline bool constantTimeEqual(const uint8_t *left, const uint8_t *right,
                              size_t size) {
  uint8_t difference = 0;
  for (size_t index = 0; index < size; ++index) {
    difference |= left[index] ^ right[index];
  }
  return difference == 0;
}

class WirelessReplayGuard {
 public:
  bool insert(const std::string &nonce) {
    for (const std::string &value : values_) {
      if (value == nonce) return false;
    }
    if (values_.size() == kWirelessSyncReplayLimit) values_.erase(values_.begin());
    values_.push_back(nonce);
    return true;
  }

  void clear() { values_.clear(); }
  size_t size() const { return values_.size(); }

 private:
  std::vector<std::string> values_;
};

enum class WirelessAuthPhase : uint8_t {
  awaitingHello,
  awaitingClientProof,
  authenticated,
  rejected,
};

class WirelessAuthPolicy {
 public:
  void reset() {
    phase_ = WirelessAuthPhase::awaitingHello;
    pairingId_.clear();
    clientNonce_.clear();
    serverNonce_.clear();
  }

  bool acceptHello(uint8_t schemaVersion, uint8_t linkVersion,
                   const std::string &requestedPairingId,
                   const std::string &expectedPairingId,
                   const std::string &clientNonce,
                   const std::string &serverNonce,
                   WirelessReplayGuard &replay) {
    if (phase_ != WirelessAuthPhase::awaitingHello || schemaVersion != 1 ||
        linkVersion != 2 || requestedPairingId != expectedPairingId ||
        !validWirelessNonce(clientNonce) ||
        !validWirelessNonce(serverNonce) || !replay.insert(clientNonce)) {
      phase_ = WirelessAuthPhase::rejected;
      return false;
    }
    pairingId_ = requestedPairingId;
    clientNonce_ = clientNonce;
    serverNonce_ = serverNonce;
    phase_ = WirelessAuthPhase::awaitingClientProof;
    return true;
  }

  bool acceptsClientProof(uint8_t schemaVersion, uint8_t linkVersion,
                          const std::string &pairingId,
                          const std::string &clientNonce,
                          const std::string &serverNonce) const {
    return phase_ == WirelessAuthPhase::awaitingClientProof &&
        schemaVersion == 1 && linkVersion == 2 && pairingId == pairingId_ &&
        clientNonce == clientNonce_ && serverNonce == serverNonce_;
  }

  void authenticated() { phase_ = WirelessAuthPhase::authenticated; }
  void reject() { phase_ = WirelessAuthPhase::rejected; }
  WirelessAuthPhase phase() const { return phase_; }
  const std::string &pairingId() const { return pairingId_; }
  const std::string &clientNonce() const { return clientNonce_; }
  const std::string &serverNonce() const { return serverNonce_; }

 private:
  WirelessAuthPhase phase_ = WirelessAuthPhase::awaitingHello;
  std::string pairingId_;
  std::string clientNonce_;
  std::string serverNonce_;
};

}  // namespace pokepod
