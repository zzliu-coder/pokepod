#pragma once

#include <Arduino.h>

#include "ProvisioningStartupPolicy.h"

namespace pokepod {

class DeviceConfig;
class ProvisioningDiagnostics;
class ProvisioningPortal;
class WifiController;

class ProvisioningCoordinator {
 public:
  bool begin(ProvisioningPortal &portal, WifiController &wifi,
             DeviceConfig &config, ProvisioningDiagnostics &diagnostics,
             Print &log);
  bool request(uint32_t nowMs);
  void poll(uint32_t nowMs);
  void stop();

  bool visible() const { return startup_.visible(); }
  bool active() const;
  bool pending() const { return startup_.pending(); }
  bool ownsWifi() const { return startup_.ownsWifi(); }
  ProvisioningStartupPhase phase() const { return startup_.phase(); }
  const char *phaseName() const {
    return provisioningStartupPhaseName(startup_.phase());
  }
  bool takeConfigurationChanged();

 private:
  void resumeNormalWifi();

  ProvisioningPortal *portal_ = nullptr;
  WifiController *wifi_ = nullptr;
  DeviceConfig *config_ = nullptr;
  ProvisioningDiagnostics *diagnostics_ = nullptr;
  Print *log_ = nullptr;
  ProvisioningStartupPolicy startup_;
  bool normalWifiResumed_ = true;
};

}  // namespace pokepod
