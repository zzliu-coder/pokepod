#include <cassert>
#include <string>
#include <vector>

#include "../PokePodAmoled/WirelessSyncProtocol.h"

int main() {
  using namespace pokepod;
  const std::string pairing = "6dc5d4b6-2618-49a4-94fb-874cfb0c6d81";
  const std::string client = "ICEiIyQlJicoKSorLC0uLw";
  const std::string server = "MDEyMzQ1Njc4OTo7PD0-Pw";
  assert(validWirelessNonce(client));
  assert(validWirelessNonce(server));
  assert(!validWirelessNonce("bad+nonce"));
  assert(wirelessProofMessage("server", pairing, client, server) ==
         "pokecapsule-v1|server|6dc5d4b6-2618-49a4-94fb-874cfb0c6d81|"
         "ICEiIyQlJicoKSorLC0uLw|MDEyMzQ1Njc4OTo7PD0-Pw");

  std::vector<uint8_t> decoded;
  assert(base64UrlDecode("AAECAwQFBgcICQoLDA0ODw", decoded));
  assert(decoded.size() == 16 && decoded.front() == 0 && decoded.back() == 15);
  assert(base64UrlEncode(decoded.data(), decoded.size()) ==
         "AAECAwQFBgcICQoLDA0ODw");

  std::vector<uint8_t> nonce15(15, 0x11);
  std::vector<uint8_t> nonce16(16, 0x22);
  std::vector<uint8_t> nonce17(17, 0x33);
  std::vector<uint8_t> nonce32(32, 0x44);
  std::vector<uint8_t> nonce33(33, 0x55);
  std::vector<uint8_t> nonce64(64, 0x66);
  std::vector<uint8_t> nonce65(65, 0x77);
  assert(!validWirelessNonce(base64UrlEncode(nonce15.data(), nonce15.size())));
  assert(validWirelessNonce(base64UrlEncode(nonce16.data(), nonce16.size())));
  assert(validWirelessNonce(base64UrlEncode(nonce17.data(), nonce17.size())));
  assert(validWirelessNonce(base64UrlEncode(nonce32.data(), nonce32.size())));
  assert(validWirelessNonce(base64UrlEncode(nonce33.data(), nonce33.size())));
  assert(validWirelessNonce(base64UrlEncode(nonce64.data(), nonce64.size())));
  assert(!validWirelessNonce(base64UrlEncode(nonce65.data(), nonce65.size())));

  WirelessReplayGuard replay;
  WirelessAuthPolicy auth;
  assert(auth.acceptHello(1, 2, pairing, pairing, client, server, replay));
  assert(auth.phase() == WirelessAuthPhase::awaitingClientProof);
  assert(auth.acceptsClientProof(1, 2, pairing, client, server));
  assert(!auth.acceptsClientProof(1, 2, pairing, client, "wrong"));
  auth.authenticated();
  assert(auth.phase() == WirelessAuthPhase::authenticated);

  auth.reset();
  assert(!auth.acceptHello(1, 2, pairing, pairing, client, server, replay));
  assert(auth.phase() == WirelessAuthPhase::rejected);

  replay.clear();
  for (size_t index = 0; index < kWirelessSyncReplayLimit + 1; ++index) {
    assert(replay.insert("nonce-" + std::to_string(index)));
  }
  assert(replay.size() == kWirelessSyncReplayLimit);
  assert(replay.insert("nonce-0"));

  uint8_t left[4] = {1, 2, 3, 4};
  uint8_t same[4] = {1, 2, 3, 4};
  uint8_t other[4] = {1, 2, 3, 5};
  assert(constantTimeEqual(left, same, 4));
  assert(!constantTimeEqual(left, other, 4));
  return 0;
}
