#pragma once

#include <cstdint>

namespace pokepod {

enum class WifiFailureKind : uint8_t {
  none,
  noAccessPoint,
  authentication,
  capacity,
  timeout,
  association,
  other,
};

inline bool isLocalWifiLeaveReason(uint16_t reason) {
  // WiFi.disconnect() can surface different local-leave reasons depending on
  // the station state. None of them describes why the preceding connection
  // attempt failed, so they must not overwrite that useful reason.
  return reason == 3 ||   // AUTH_LEAVE
         reason == 8 ||   // ASSOC_LEAVE
         reason == 36;    // STA_LEAVING
}

inline WifiFailureKind wifiFailureKind(uint16_t reason) {
  switch (reason) {
    case 0: return WifiFailureKind::none;
    case 201:  // NO_AP_FOUND
    case 212:  // NO_AP_FOUND_IN_RSSI_THRESHOLD
      return WifiFailureKind::noAccessPoint;
    case 2:    // AUTH_EXPIRE
    case 6:    // NOT_AUTHED
    case 7:    // NOT_ASSOCED
    case 15:   // 4WAY_HANDSHAKE_TIMEOUT
    case 23:   // 802_1X_AUTH_FAILED
    case 202:  // AUTH_FAIL
    case 204:  // HANDSHAKE_TIMEOUT
    case 210:  // NO_AP_FOUND_W_COMPATIBLE_SECURITY
    case 211:  // NO_AP_FOUND_IN_AUTHMODE_THRESHOLD
      return WifiFailureKind::authentication;
    case 5: return WifiFailureKind::capacity;  // ASSOC_TOOMANY
    case 39:   // TIMEOUT
    case 200:  // BEACON_TIMEOUT
      return WifiFailureKind::timeout;
    case 203:  // ASSOC_FAIL
    case 205:  // CONNECTION_FAIL
      return WifiFailureKind::association;
    default: return WifiFailureKind::other;
  }
}

inline const char *wifiFailureKey(uint16_t reason) {
  switch (wifiFailureKind(reason)) {
    case WifiFailureKind::none: return "none";
    case WifiFailureKind::noAccessPoint: return "no_access_point";
    case WifiFailureKind::authentication: return "authentication";
    case WifiFailureKind::capacity: return "capacity";
    case WifiFailureKind::timeout: return "timeout";
    case WifiFailureKind::association: return "association";
    case WifiFailureKind::other: return "other";
  }
  return "other";
}

}  // namespace pokepod
