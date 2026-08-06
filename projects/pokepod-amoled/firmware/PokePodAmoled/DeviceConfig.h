#pragma once

#include <Arduino.h>
#include <Preferences.h>

namespace pokepod {

struct DeviceSettings {
  String wifiSsid;
  String wifiPassword;
  String secretId;
  String secretKey;
  String hotwordId;
  bool wifiEnabled = true;
  bool raiseToWake = true;
};

class DeviceConfig {
 public:
  bool begin(Print &log);
  bool save(const DeviceSettings &settings, Print &log);
  bool clearWifi(Print &log);
  bool clearTencent(Print &log);

  const DeviceSettings &settings() const { return settings_; }
  bool hasWifi() const { return !settings_.wifiSsid.isEmpty(); }
  bool hasTencent() const {
    return !settings_.secretId.isEmpty() && !settings_.secretKey.isEmpty();
  }
  bool setWifiEnabled(bool enabled, Print &log);
  bool setRaiseToWake(bool enabled, Print &log);

 private:
  Preferences preferences_;
  DeviceSettings settings_;
  bool open_ = false;
};

}  // namespace pokepod
