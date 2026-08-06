#pragma once

#include <Arduino.h>
#include <DNSServer.h>
#include <WebServer.h>

#include "DeviceConfig.h"

namespace pokepod {

class ProvisioningPortal {
 public:
  ProvisioningPortal();
  bool begin(DeviceConfig &config, Print &log);
  void loop(uint32_t nowMs);
  void stop();
  bool active() const { return active_; }
  const String &ssid() const { return ssid_; }
  const String &password() const { return password_; }
  bool takeConfigurationChanged();

 private:
  void installRoutes();
  void showPortal();
  void saveRequest();
  void redirectPortal();
  String pageHtml() const;
  static String htmlEscape(const String &value);

  DNSServer dns_;
  WebServer server_;
  DeviceConfig *config_ = nullptr;
  Print *log_ = nullptr;
  DeviceSettings candidate_;
  String ssid_;
  String password_;
  String statusMessage_;
  bool routesInstalled_ = false;
  bool active_ = false;
  bool validating_ = false;
  bool changed_ = false;
  uint32_t startedMs_ = 0;
  uint32_t validatingSinceMs_ = 0;
  uint32_t closeAtMs_ = 0;
};

}  // namespace pokepod
