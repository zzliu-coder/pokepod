#include <cassert>
#include <cstring>

#include "DeviceConfigBlob.h"

using namespace pokepod;

int main() {
  const char vector[] = "123456789";
  assert(deviceConfigCrc32(reinterpret_cast<const uint8_t *>(vector), 9) ==
         0xcbf43926U);

  StoredDeviceConfig empty{};
  empty.magic = kDeviceConfigMagic;
  empty.version = kDeviceConfigVersion;
  empty.wifiEnabled = 1;
  empty.raiseToWake = 1;
  finalizeDeviceConfigBlob(empty);
  assert(validateDeviceConfigBlob(empty));

  StoredDeviceConfig one = empty;
  one.wifiCount = 1;
  std::strcpy(one.wifi[0].ssid, "example");
  std::strcpy(one.wifi[0].password, "12345678");
  finalizeDeviceConfigBlob(one);
  assert(validateDeviceConfigBlob(one));

  StoredDeviceConfig corrupted = one;
  corrupted.wifi[0].password[0] ^= 1;
  assert(!validateDeviceConfigBlob(corrupted));

  StoredDeviceConfig future = one;
  future.version = kDeviceConfigVersion + 1;
  finalizeDeviceConfigBlob(future);
  assert(!validateDeviceConfigBlob(future));

  StoredDeviceConfig tooMany = empty;
  tooMany.wifiCount = kMaximumRememberedWifiNetworks + 1;
  finalizeDeviceConfigBlob(tooMany);
  assert(!validateDeviceConfigBlob(tooMany));

  StoredDeviceConfig unterminated = one;
  std::memset(unterminated.wifi[0].ssid, 'x',
              sizeof(unterminated.wifi[0].ssid));
  finalizeDeviceConfigBlob(unterminated);
  assert(!validateDeviceConfigBlob(unterminated));

  return 0;
}
