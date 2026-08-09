#include "WirelessSyncBonjour.h"

#include <ESPmDNS.h>
#include <esp_random.h>

#include "WirelessSyncProtocol.h"

namespace pokepod {

bool WirelessSyncBonjour::start(uint16_t port, bool paired, Print &log) {
  stop();
  uint8_t nonce[16];
  esp_fill_random(nonce, sizeof(nonce));
  instanceNonce_ = base64UrlEncode(nonce, sizeof(nonce)).c_str();
  char host[22];
  snprintf(host, sizeof(host), "sync-%02x%02x%02x%02x%02x%02x",
           nonce[0], nonce[1], nonce[2], nonce[3], nonce[4], nonce[5]);
  if (!MDNS.begin(host)) {
    instanceNonce_ = "";
    return false;
  }
  MDNS.setInstanceName(host);
  if (!MDNS.addService("pokecapsule", "tcp", port)) {
    MDNS.end();
    instanceNonce_ = "";
    return false;
  }
  MDNS.addServiceTxt("pokecapsule", "tcp", "schema", "1");
  MDNS.addServiceTxt("pokecapsule", "tcp", "v", "2");
  MDNS.addServiceTxt("pokecapsule", "tcp", "instance",
                     instanceNonce_.c_str());
  MDNS.addServiceTxt("pokecapsule", "tcp", "paired", paired ? "1" : "0");
  MDNS.addServiceTxt("pokecapsule", "tcp", "caps",
                     "link-v2,tls,hmac-sha256,resume");
  active_ = true;
  pairedAdvertised_ = paired;
  log.printf("{\"event\":\"wifi_sync_bonjour\",\"active\":true,\"paired\":%s}\n",
             paired ? "true" : "false");
  return true;
}

void WirelessSyncBonjour::stop() {
  if (active_) MDNS.end();
  active_ = false;
  pairedAdvertised_ = false;
  instanceNonce_ = "";
}

}  // namespace pokepod
