#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include <vector>

#include "DeviceConfigBlob.h"

namespace pokepod {

struct DeviceSettings {
  String wifiSsid;
  String wifiPassword;
  String secretId;
  String secretKey;
  String hotwordId;
  bool wifiEnabled = true;
  bool bluetoothEnabled = true;
  bool raiseToWake = true;
  ProvisioningPasswordMode provisioningPasswordMode =
      kDefaultProvisioningPasswordMode;
};

struct WifiCredential {
  String ssid;
  String password;
};

class DeviceConfig {
 public:
  bool begin(Print &log);
  bool save(const DeviceSettings &settings, Print &log);
  bool clearWifi(Print &log);
  bool clearTencent(Print &log);
  bool forgetWifi(const String &ssid, Print &log);
  bool markWifiSuccessful(const String &ssid, Print &log);

  const DeviceSettings &settings() const { return settings_; }
  const std::vector<WifiCredential> &wifiNetworks() const {
    return wifiNetworks_;
  }
  const WifiCredential *wifiNetwork(const String &ssid) const;
  bool hasWifi() const { return !wifiNetworks_.empty(); }
  bool hasTencent() const {
    return !settings_.secretId.isEmpty() && !settings_.secretKey.isEmpty();
  }
  bool setWifiEnabled(bool enabled, Print &log);
  bool setBluetoothEnabled(bool enabled, Print &log);
  bool setRaiseToWake(bool enabled, Print &log);
  bool setProvisioningPasswordMode(ProvisioningPasswordMode mode, Print &log);

 private:
  bool persistState(const DeviceSettings &settings,
                    const std::vector<WifiCredential> &networks);
  bool rememberWifi(const String &ssid, const String &password, Print &log);
  void syncPreferredWifi();

  Preferences preferences_;
  DeviceSettings settings_;
  std::vector<WifiCredential> wifiNetworks_;
  bool open_ = false;
};

}  // namespace pokepod
