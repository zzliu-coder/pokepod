#pragma once

#include <Arduino.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <vector>

#include "DeviceConfig.h"
#include "ProvisioningDiagnostics.h"
#include "ProvisioningPolicy.h"
#include "WirelessSecurityPolicy.h"

namespace pokepod {

class ProvisioningPortal {
 public:
  ProvisioningPortal();
  bool prepare(DeviceConfig &config, ProvisioningDiagnostics &diagnostics,
               Print &log);
  bool switchToAccessPointMode();
  bool startAccessPoint();
  bool startServices();
  void failStartupTimeout();
  void loop(uint32_t nowMs);
  void stop();
  bool active() const { return active_; }
  bool prepared() const { return prepared_; }
  const String &ssid() const { return ssid_; }
  const String &password() const { return password_; }
  const String &statusMessage() const { return statusMessage_; }
  ProvisioningState state() const;
  bool takeConfigurationChanged();

 private:
  struct ScannedNetwork {
    String ssid;
    int32_t rssi = -127;
    bool secured = true;
  };

  void installRoutes();
  void startScan();
  void pollScan();
  void showNetworks();
  void scanRequest();
  void showPortal();
  void saveRequest();
  void forgetRequest();
  bool authorizeMutation();
  void sendSaveJson(int statusCode, bool accepted);
  void beginStationValidation();
  void restorePortalForRetry();
  const char *portalState() const;
  void redirectPortal();
  String pageHtml() const;
  String networksJson() const;
  static String htmlEscape(const String &value);
  static String jsonEscape(const String &value);

  DNSServer dns_;
  WebServer server_;
  DeviceConfig *config_ = nullptr;
  ProvisioningDiagnostics *diagnostics_ = nullptr;
  Print *log_ = nullptr;
  DeviceSettings candidate_;
  String ssid_;
  String password_;
  String statusMessage_;
  std::vector<ScannedNetwork> networks_;
  bool routesInstalled_ = false;
  bool prepared_ = false;
  bool starting_ = false;
  bool active_ = false;
  bool validating_ = false;
  bool scanning_ = false;
  bool transitionPending_ = false;
  bool changed_ = false;
  bool saved_ = false;
  uint32_t startedMs_ = 0;
  uint32_t validatingSinceMs_ = 0;
  uint32_t scanStartedMs_ = 0;
  uint32_t transitionAtMs_ = 0;
  uint32_t closeAtMs_ = 0;
  int16_t candidateRssi_ = -127;
  uint8_t validationAttempt_ = 0;
  ProvisioningCsrfPolicy csrf_;
};

}  // namespace pokepod
