#include <cassert>
#include <string>

#include "WifiFailurePolicy.h"

using namespace pokepod;

int main() {
  assert(isLocalWifiLeaveReason(3));
  assert(isLocalWifiLeaveReason(8));
  assert(isLocalWifiLeaveReason(36));
  assert(!isLocalWifiLeaveReason(201));
  assert(wifiFailureKind(0) == WifiFailureKind::none);
  assert(wifiFailureKind(201) == WifiFailureKind::noAccessPoint);
  assert(wifiFailureKind(202) == WifiFailureKind::authentication);
  assert(wifiFailureKind(210) == WifiFailureKind::authentication);
  assert(wifiFailureKind(212) == WifiFailureKind::noAccessPoint);
  assert(wifiFailureKind(15) == WifiFailureKind::authentication);
  assert(wifiFailureKind(5) == WifiFailureKind::capacity);
  assert(wifiFailureKind(200) == WifiFailureKind::timeout);
  assert(wifiFailureKind(203) == WifiFailureKind::association);
  assert(wifiFailureKind(999) == WifiFailureKind::other);
  assert(std::string(wifiFailureKey(204)) == "authentication");
  return 0;
}
