#include "WifiDisconnectDiagnostics.h"

#include "WifiFailurePolicy.h"

#include <WiFi.h>
#include <atomic>

namespace pokepod {
namespace {

std::atomic<uint16_t> gLastDisconnectReason{0};
bool gInstalled = false;

}  // namespace

void beginWifiDisconnectDiagnostics() {
  if (gInstalled) return;
  WiFi.onEvent([](arduino_event_id_t event, arduino_event_info_t info) {
    if (event != ARDUINO_EVENT_WIFI_STA_DISCONNECTED) return;
    const uint16_t reason = info.wifi_sta_disconnected.reason;
    // WiFi.disconnect() emits a local leave after the useful failure event.
    // Keep the meaningful failure instead of overwriting it during cleanup.
    if (reason != 0 && !isLocalWifiLeaveReason(reason)) {
      gLastDisconnectReason.store(reason, std::memory_order_relaxed);
    }
  }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  gInstalled = true;
}

void clearWifiDisconnectReason() {
  gLastDisconnectReason.store(0, std::memory_order_relaxed);
}

uint16_t lastWifiDisconnectReason() {
  return gLastDisconnectReason.load(std::memory_order_relaxed);
}

}  // namespace pokepod
