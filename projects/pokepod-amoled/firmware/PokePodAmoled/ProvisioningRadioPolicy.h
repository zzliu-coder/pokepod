#pragma once

namespace pokepod {

// WiFi.mode(WIFI_OFF) is synchronous in Arduino-ESP32, while WiFi.status()
// is an event-updated STA cache that may still report WL_CONNECTED after the
// radio has stopped. Provisioning readiness therefore depends on the result of
// the mode transition, the actual mode, and the scan state only.
inline bool provisioningRadioReady(bool modeOffSucceeded, bool modeIsOff,
                                   bool scanRunning) {
  return modeOffSucceeded && modeIsOff && !scanRunning;
}

}  // namespace pokepod
