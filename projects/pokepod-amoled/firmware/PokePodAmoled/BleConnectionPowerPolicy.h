#pragma once

#include <stdint.h>

namespace pokepod {

enum class BleConnectionPowerMode : uint8_t {
  idle,
  voice,
};

// Arduino-ESP32 3.3.8's ESP32-S3 controller can watchdog inside
// ll_connection_update_ind when an application-initiated update overlaps the
// peer/controller procedure. PokePod's bounded audio queue tolerates the
// negotiated default interval, so the locked production core must not issue
// an additional update from firmware.
inline constexpr bool bleFirmwareMayRequestConnectionParameters() {
  return false;
}

struct BleConnectionParameters {
  uint16_t minInterval;
  uint16_t maxInterval;
  uint16_t latency;
  uint16_t timeout;
};

inline BleConnectionParameters bleConnectionParameters(
    BleConnectionPowerMode mode) {
  if (mode == BleConnectionPowerMode::voice) {
    // 1.25 ms units: keep every connection event available while streaming.
    return {12, 16, 0, 500};
  }
  // 30-40 ms connection interval with slave latency 4 gives a 150-200 ms
  // effective idle wake cadence. The 480 ms firmware audio queue absorbs the
  // asynchronous transition back to the voice parameters.
  return {24, 32, 4, 500};
}

}  // namespace pokepod
