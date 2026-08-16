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
  // Existing v1 blobs used zero-filled reserved bytes. They migrate to the
  // historical enabled behavior without changing blob size or version.
  assert(empty.reserved[0] == kStoredBluetoothLegacyEnabled);
  assert(storedDeviceConfigBluetoothEnabled(empty));
  // Zero-filled v1 blobs remain the legacy/random provisioning behavior.
  assert(storedDeviceConfigProvisioningPasswordMode(empty) ==
         ProvisioningPasswordMode::legacy);
  assert(migrateProvisioningPasswordMode(
             ProvisioningPasswordMode::legacy) ==
         ProvisioningPasswordMode::fixed88888888);
  assert(kDefaultProvisioningPasswordMode ==
         ProvisioningPasswordMode::fixed88888888);

  StoredDeviceConfig random = empty;
  random.reserved[1] = kStoredProvisioningPasswordRandom;
  finalizeDeviceConfigBlob(random);
  assert(validateDeviceConfigBlob(random));
  assert(storedDeviceConfigProvisioningPasswordMode(random) ==
         ProvisioningPasswordMode::random);

  StoredDeviceConfig fixed = empty;
  fixed.reserved[1] = kStoredProvisioningPasswordFixed88888888;
  finalizeDeviceConfigBlob(fixed);
  assert(validateDeviceConfigBlob(fixed));
  assert(storedDeviceConfigProvisioningPasswordMode(fixed) ==
         ProvisioningPasswordMode::fixed88888888);

  StoredDeviceConfig invalidProvisioningMode = empty;
  invalidProvisioningMode.reserved[1] =
      kStoredProvisioningPasswordFixed88888888 + 1;
  finalizeDeviceConfigBlob(invalidProvisioningMode);
  assert(!validateDeviceConfigBlob(invalidProvisioningMode));

  StoredDeviceConfig bluetoothOn = empty;
  bluetoothOn.reserved[0] = kStoredBluetoothEnabled;
  finalizeDeviceConfigBlob(bluetoothOn);
  assert(validateDeviceConfigBlob(bluetoothOn));
  assert(storedDeviceConfigBluetoothEnabled(bluetoothOn));

  StoredDeviceConfig bluetoothOff = empty;
  bluetoothOff.reserved[0] = kStoredBluetoothDisabled;
  finalizeDeviceConfigBlob(bluetoothOff);
  assert(validateDeviceConfigBlob(bluetoothOff));
  assert(!storedDeviceConfigBluetoothEnabled(bluetoothOff));

  StoredDeviceConfig invalidBluetooth = empty;
  invalidBluetooth.reserved[0] = kStoredBluetoothDisabled + 1;
  finalizeDeviceConfigBlob(invalidBluetooth);
  assert(!validateDeviceConfigBlob(invalidBluetooth));

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

  assert(validProvisioningPasswordMode(ProvisioningPasswordMode::legacy));
  assert(validProvisioningPasswordMode(ProvisioningPasswordMode::random));
  assert(validProvisioningPasswordMode(
      ProvisioningPasswordMode::fixed88888888));

  return 0;
}
